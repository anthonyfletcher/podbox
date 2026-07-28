/***************************************************************************
 * Original code from RockBox
 * was: apps/action.c
 * Copyright (C) 2006 Jonathan Gordon
 * Copyright (C) 2017 William Wilgus
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Turns raw button events into context-sensitive actions: applies the
 * keymap for the current context, handles repeat and long-press, and
 * drives the backlight and keyclick.
 *
 * A keymap is a flat table of {action, button, prerequisite-button} rows,
 * searched in order for the first row matching the button just pressed. The
 * prerequisite field is what expresses "this combination" and "this button
 * after that one"; BUTTON_NONE means no prerequisite. Contexts fall back to
 * one another, so a context need only list what it changes and the search
 * continues into the parent context on no match.
 *
 * Long presses cannot be recognised on press, only on release or on the
 * repeat timer, so get_action() holds state between calls -- the last button,
 * when it went down, and whether its action has already been reported. That
 * state is why the action layer must be driven from one place and why
 * get_action() is not reentrant.
 *
 * Parts, in order:
 *   - the pending-action state carried between calls
 *   - keymap lookup and context fallback
 *   - press, repeat, release and long-press timing
 *   - software keylock and the action filters
 *   - get_action()/get_custom_action() and the touch/scroll helpers
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "lang.h"

#include "speech/language.h"

#include "system/appevents.h"
#include "button.h"
#include "action.h"
#include "kernel.h"
#include "core_alloc.h"

#include "widgets/splash.h"
#include "settings/settings.h"
#include "system/app_util.h"
#include "audio/sound_feedback.h"
#include "system/shutdown.h"
#include "skin/skin_engine.h" /* skin_inhibit_flush */


#include "backlight.h"
#include "power.h"

/*#define LOGF_ENABLE*/
#include "logf.h"

#define REPEAT_WINDOW_TICKS HZ/4
#define ACTION_FILTER_TICKS HZ/2 /* timeout between filtered actions SL/BL */

/* act_cur holds action state during get_action() call */
typedef struct
{
    int                            action;
    int                            button;
    int                            context;
    int                            timeout;
    const struct button_mapping   *items;
    const struct button_mapping* (*get_context_map)(int);
    bool                           is_prebutton;
} action_cur_t;

/* act_last holds action state between get_action() calls */
typedef struct
{
    int      action;
    long     tick;
    int      button;
    int      context;
    intptr_t data;

    unsigned int backlight_mask;
    long         bl_filter_tick;


    bool          repeated;
    bool          wait_for_release;

#ifndef DISABLE_ACTION_REMAP
    int     key_remap;
#endif

} action_last_t;

/* holds the action state between calls to get_action \ get_action_custom) */
static action_last_t action_last =
{
    .action           = ACTION_NONE,
    .button           = BUTTON_NONE | BUTTON_REL, /* allow the ipod wheel to
                                                     work on startup */
    .context          = CONTEXT_STD,
    .data             = 0,
    .repeated         = false,
    .tick             = 0,
    .wait_for_release = false,

#ifndef DISABLE_ACTION_REMAP
    .key_remap = 0,
#endif


    .backlight_mask = SEL_ACTION_NONE,
    .bl_filter_tick = 0,

}; /* action_last_t action_last */

/******************************************************************************
** INTERNAL ACTION FUNCTIONS **************************************************
*******************************************************************************
*/

/******************************************
* has_flag compares value to a (SINGLE) flag
* returns true if set, false otherwise
*/
static inline bool has_flag(unsigned int value, unsigned int flag)
{
    return ((value & flag) == flag);
}

/* HELPER FUNCTIONS selective softlock and backlight */

/****************************************************************
* is_action_filtered, selective softlock and backlight use this
* to lookup which actions are filtered, matches are only true if
* action is found and supplied SEL_ACTION mask has the flag.
* returns false if the action isn't found or isn't enabled,
* true if the action is found and is enabled
*/
static bool is_action_filtered(int action, unsigned int mask, int context)
{
    /* context is now only read by the logf() in the default case below, which
     * compiles away on targets without ROCKBOX_HAS_LOGF (ipodvideo). It used
     * to be read unconditionally by the CONTEXT_FM test, which is gone with
     * the tuner. Keep the parameter -- callers pass it and logf builds use it
     * -- but silence the unused warning on the builds that do not. */
    (void)context;

    bool match = false;

    switch (action)
    {
        case ACTION_NONE:
            break;
        /* Actions that are not mapped will not turn on the backlight */
        case ACTION_UNKNOWN:
            match = has_flag(mask, SEL_ACTION_NOUNMAPPED);
            break;
        case ACTION_WPS_PLAY:
            match = has_flag(mask, SEL_ACTION_PLAY);
            break;
        /* case ACTION_STD_PREVREPEAT:*/ /* seek not exempted outside of WPS */
        /* case ACTION_STD_NEXTREPEAT: */
        case ACTION_WPS_SEEKBACK:
        case ACTION_WPS_SEEKFWD:
        case ACTION_WPS_STOPSEEK:
            match = has_flag(mask, SEL_ACTION_SEEK);
            break;
        /* case ACTION_STD_PREV: */ /* skip/scrollwheel not */
        /* case ACTION_STD_NEXT: */ /* exempted outside of WPS */
        case ACTION_WPS_SKIPNEXT:
        case ACTION_WPS_SKIPPREV:
            match = has_flag(mask, SEL_ACTION_SKIP);
            break;
        case ACTION_WPS_VOLUP:
        case ACTION_WPS_VOLDOWN:
            match = has_flag(mask, SEL_ACTION_VOL);
            break;
        case ACTION_SETTINGS_INC:
        case ACTION_SETTINGS_INCREPEAT:
        case ACTION_SETTINGS_DEC:
        case ACTION_SETTINGS_DECREPEAT:
            /* Never a volume action here. Listed explicitly rather than left
             * to default:, which logs every action it sees as unfiltered. */
            match = false;
            break;
        default:
            /* display action code of unfiltered actions */
            logf ("unfiltered actions: context: %d action: %d, last btn: %d, \
                  mask: %d", context, action, action_last.button, mask);
            break;
    }/*switch*/

    return match;
}

/*******************************************************************************
* is_action_discarded:
* Most every action takes two rounds through get_action_worker,
* once for the keypress and once for the key release,
* actions with pre_button codes take even more, some actions however, only
* take once; actions defined with only a button and no release/repeat event,
* these actions should be acted upon immediately except when we have
* selective backlighting/softlock enabled and in this case we only act upon
* them immediately if there is no chance they have another event tied to them
* determined using !is_prebutton or if action is completed
* returns true if event was discarded and false if it was kept
*/
static bool is_action_discarded(action_cur_t *cur, bool filtered, long *tick)
{
    bool ret = true;
    bool completed = (cur->button & (BUTTON_REPEAT | BUTTON_REL)) != 0;

    /* Scrollwheel doesn't generate release events  */
    completed |= (cur->button & (BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD)) != 0;

    /*directly after a match a key release event may trigger another*/
    if (filtered && cur->action != ACTION_UNKNOWN)
    {
        *tick = current_tick + ACTION_FILTER_TICKS;
    }
    /* has button been released/repeat or is this the only action it could be */
    if (completed || !cur->is_prebutton)
    {
        /* if the action is not filtered and this isn't just a
        * key release event then return false
        * keeping action, and reset tick
        */
        if (!filtered && *tick < current_tick)
        {
            *tick = 0;
             ret  = false;
        }
    }

    return ret;
}

/*******************************************************
* action_handle_backlight is used to both delay
* and activate the backlight if HAVE_BACKLIGHT
* and SEL_ACTION_ENABLED; backlight state is
* set true/false and ignore_next sets the backlight
* driver to ignore backlight_on commands from
* other modules for a finite duration;
* Ignore is set each time the action system
* handles the backlight as a precaution since, if
* the action system was not triggered the device would
* appear unresponsive to the user.
* If a backlight_on event hasn't been handled in the
* ignore duration it will timeout and the next call
* to backlight_on will trigger as normal
*/
static void action_handle_backlight(bool backlight, bool ignore_next)
{
    if (backlight)
    {
        backlight_on_ignore(false, 0);
        backlight_on();
    }

    backlight_on_ignore(ignore_next, 5*HZ);/*must be set everytime we handle bl*/


}


/******************************************************************
* action_poll_button filters button presses for get_action_worker;
* if button_get_w_tmo returns...
* BUTTON_NONE, SYS_EVENTS, MULTIMEDIA BUTTONS, ACTION_REDRAW
* they are allowed to pass immediately through to handler.
* if waiting for button release ACTION_NONE is returned until
* button is released/repeated.
*/
static inline bool action_poll_button(action_last_t *last, action_cur_t *cur)
{
    bool ret = true;
    int *button = &cur->button;

    *button = button_get_w_tmo(cur->timeout);

   /* ********************************************************
    * Can return button immediately, sys_event & multimedia
    * button presses don't use the action system, Data from
    * sys events can be pulled with button_get_data.
    * BUTTON_REDRAW should result in a screen refresh
    */
    if (*button == BUTTON_NONE || (*button & (SYS_EVENT|BUTTON_MULTIMEDIA)) != 0)
    {
        return true;
    }
    else if (*button == BUTTON_REDRAW)
    {   /* screen refresh */
        *button = ACTION_REDRAW;
        return true;
    }
   /* *************************************************
    * If waiting for release, Don't send any buttons
    * through until we see the release event
    */
    if (last->wait_for_release)
    {
        if (has_flag(*button, BUTTON_REL))
        { /* remember the button for button eating on context change */
            last->wait_for_release = false;
            last->button = *button;
        }

        *button = ACTION_NONE;
    }
   /* *********************************************
    * Scrollwheel doesn't generate release events
    * further processing needed
    */
    else if ((last->button & (BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD)) != 0)
    {
        ret = false;
    }
   /* *************************************************************
    * On Context Changed eat all buttons until the previous button
    * was |BUTTON_REL (also eat the |BUTTON_REL button)
    */
    else if ((cur->context != last->context) && ((last->button & BUTTON_REL) == 0))
    {
        if (has_flag(*button, BUTTON_REL))
        {
            last->button = *button;
            last->action = ACTION_NONE;
            *button = ACTION_NONE;
        }
        else if ((*button & (BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD)) != 0)
        {
            /* Scrollwheel has no release events so it can never satisfy the
             * context-change gate.  Let it through and update the context. */
            ret = false;
        }
        else
        {
            *button = ACTION_NONE;
        }
    }
   /* ****************************
    * regular button press,
    * further processing needed
    */
    else
    {
        ret = false;
    }

    /* current context might contain ALLOW_SOFTLOCK save prior to stripping it */
    if (!ret)
    {
        last->context = cur->context;
    }

    return ret;
}

/*********************************************
* update_screen_has_lock sets screen_has_lock
* if passed context contains ALLOW_SOFTLOCK
* and removes ALLOW_SOFTLOCK from the passed
* context flag
*/
static inline void update_screen_has_lock(action_last_t *last, action_cur_t *cur)
{
    (void) last;
    (void) cur;
    return;
}

/***********************************************
* handles touch event processing
*/
static inline bool get_action_touchscreen(action_last_t *last, action_cur_t *cur)
{
    (void) last;
    (void) cur;
    return false;
}

/******************************************************************************
* button_flip_horizontally, passed button is horizontally inverted to support
* RTL language if the given language and context combination require it
* Affected contexts: CONTEXT_STD, CONTEXT_TREE, CONTEXT_LIST, CONTEXT_MAINMENU
* Affected buttons with rtl language:
* BUTTON_LEFT, BUTTON_RIGHT,
* Affected buttons with rtl language and !simulator:
* BUTTON_SCROLL_BACK, BUTTON_SCROLL_FWD, BUTTON_MINUS, BUTTON_PLUS
*/
static inline void button_flip_horizontally(int context, int *button)
{

    int newbutton = *button;
    if (!(lang_is_rtl() && ((context == CONTEXT_STD) ||
        (context == CONTEXT_TREE) || (context == CONTEXT_LIST) ||
        (context == CONTEXT_MAINMENU))))
    {
        return;
    }

#if defined(BUTTON_LEFT) && defined(BUTTON_RIGHT)
    newbutton &= ~(BUTTON_LEFT | BUTTON_RIGHT);
    if (has_flag(*button, BUTTON_LEFT))
    {
        newbutton |= BUTTON_RIGHT;
    }

    if (has_flag(*button, BUTTON_RIGHT))
    {
        newbutton |= BUTTON_LEFT;
    }
#else
#warning "BUTTON_LEFT / BUTTON_RIGHT not defined!"
#endif

    newbutton &= ~(BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD);
    if (has_flag(*button, BUTTON_SCROLL_BACK))
    {
        newbutton |= BUTTON_SCROLL_FWD;
    }

    if (has_flag(*button, BUTTON_SCROLL_FWD))
    {
        newbutton |= BUTTON_SCROLL_BACK;
    }


    *button = newbutton;
} /* button_flip_horizontally */

/**********************************************************************
* action_code_worker is the worker function for action_code_lookup.
* returns ACTION_UNKNOWN or the requested return value from the list.
* BE AWARE IF YOUR DESIRED ACTION IS IN A LOWER 'CHAINED' CONTEXT::
* *** is_prebutton can miss pre_buttons
* ** An action without pre_button_code (pre_button_code = BUTTON_NONE)
* *  will be returned from the higher context
*/
static inline int action_code_worker(action_last_t *last,
                                     action_cur_t  *cur,
                                              int  *end  )
{
    int ret = ACTION_UNKNOWN;
    int i = *end;
    unsigned int found = 0;
    while (cur->items[i].button_code != BUTTON_NONE)
    {
        if (cur->items[i].button_code == cur->button)
        {
            /********************************************************
            * { Action Code,   Button code,    Prereq button code }
            * CAVEAT: This will allways return the action without
            * pre_button_code (pre_button_code = BUTTON_NONE)
            * if it is found before 'falling through'
            * to a lower 'chained' context.
            *
            * Example: button = UP|REL, last_button = UP;
            *  while looking in CONTEXT_WPS there is an action defined
            *  {ACTION_FOO, BUTTON_UP|BUTTON_REL, BUTTON_NONE}
            *  then ACTION_FOO in CONTEXT_WPS will be returned
            *  EVEN THOUGH you are expecting a fully matched
            *  ACTION_BAR from CONTEXT_STD
            *  {ACTION_BAR, BUTTON_UP|BUTTON_REL, BUTTON_UP}
            */
            if (cur->items[i].pre_button_code == last->button)
            {   /* Always allow an exact match */
                found++;
                *end = i;
            }
            else if (!found && cur->items[i].pre_button_code == BUTTON_NONE)
            {   /* Only allow Loose match if exact match wasn't found */
                found++;
                *end = i;
            }
        }
        else if (has_flag(cur->items[i].pre_button_code, cur->button))
        { /* This could be another action depending on next button press */
            cur->is_prebutton = true;
            if (found > 1) /* There is already an exact match */
            {
                break;
            }
        }
        i++;
    }

    if (!found)
    {
        *end = i;
    }
    else
    {
        ret = cur->items[*end].action_code;
    }

    return ret;
}

/***************************************************************************
* get_next_context returns the next CONTEXT to be searched for action_code
* by action_code_lookup(); if needed it first continues incrementing till
* the end of current context map is reached; If there is another
* 'chained' context below the current context this new context is returned
* if there is not a 'chained' context to return, CONTEXT_STD is returned;
*/
static inline int get_next_context(const struct button_mapping *items, int i)
{
    while (items[i].button_code != BUTTON_NONE)
    {
        i++;
    }

    return (items[i].action_code == ACTION_NONE ) ?
            CONTEXT_STD : items[i].action_code;
}

/************************************************************************
* action_code_lookup passes current button, last button and is_prebutton
* to action_code_worker() which uses the current button map to
* lookup action_code.
* BE AWARE IF YOUR DESIRED ACTION IS IN A LOWER 'CHAINED' CONTEXT::
* *** is_prebutton can miss pre_buttons
* ** An action without pre_button_code (pre_button_code = BUTTON_NONE)
* *  will be returned from the higher context see action_code_worker()
*  for a more in-depth explanation
* places action into current_action
*/

static inline void action_code_lookup(action_last_t *last, action_cur_t *cur)
{
    int  action, i;
    int  context = cur->context;
    cur->is_prebutton = false;


#ifndef DISABLE_ACTION_REMAP
        /* attempt to look up the button in user supplied remap */
        if(last->key_remap && (context & CONTEXT_PLUGIN) == 0)
        {
            if ((cur->button & BUTTON_REMOTE) != 0)
            {
                context |= CONTEXT_REMOTE;
            }
            cur->items = core_get_data(last->key_remap);
            i = 0;
            action = ACTION_UNKNOWN;
            /* check the lut at the beginning for the desired context */
            while (cur->items[i].button_code != BUTTON_NONE)
            {
                if (cur->items[i].action_code == CORE_CONTEXT_REMAP(context))
                {
                    i = cur->items[i].button_code;
                    action = action_code_worker(last, cur, &i);
                    if (action != ACTION_UNKNOWN)
                    {
                        cur->action = action;
                        return;
                    }
                }
                i++;
            }
        }
#endif

    i = 0;
    action  = ACTION_NONE;
    /* attempt to look up the button in the in-built keymaps */
    for(;;)
    {
        /* logf("context = %x",context); */
#if (BUTTON_REMOTE != 0)
        if ((cur->button & BUTTON_REMOTE) != 0)
        {
            context |= CONTEXT_REMOTE;
        }
#endif

        if ((context & CONTEXT_PLUGIN) && cur->get_context_map)
        {
            cur->items = cur->get_context_map(context);
        }
        else
        {
            cur->items = get_context_mapping(context);
        }

        if (cur->items != NULL)
        {
            action = action_code_worker(last, cur, &i);

            if (action == ACTION_UNKNOWN)
            {
                context = get_next_context(cur->items, i);

                if (context != (int)CONTEXT_STOPSEARCHING)
                {
                    i = 0;
                    continue;
                }
            }
        }
        /* No more items, action was found, or STOPSEARCHING was specified */
        break;
    }
    cur->action = action;
}


/*****************************************************
* do_softlock Handles softlock once action is known
* selective softlock allows user selected actions to
* bypass a currently locked state, special lock state
* autolock is handled here as well if HAVE_BACKLIGHT
*/
static inline void do_softlock(action_last_t *last, action_cur_t *cur)
{
    (void) last;
    (void) cur;
    return;
}

/**********************************************************************
* update_action_last copies the current action values into action_last
* saving the current state & allowing get_action_worker() to return
* while waiting for the next button press; Since some actions take
* multiple buttons, this allows those actions to be looked up and
* returned in a non-blocking way;
* Returns action, checks\sets repeated, plays keyclick (if applicable)
*/
static inline int update_action_last(action_last_t *last, action_cur_t *cur)
{
    int  action = cur->action;

    logf ("action system: context: %d last context: %d, action: %d, \
           last action: %d, button %d, last btn: %d, last repeated: %d, \
           last_data: %d", cur->context, last->context, cur->action,
           last->action, cur->button, last->button, last->repeated, last->data);

    if (action == last->action)
    {
        last->repeated = (current_tick < last->tick + REPEAT_WINDOW_TICKS);
    }
    else
    {
        last->repeated = false;
    }

    last->action = action;
    last->button = cur->button;
    last->data   = button_get_data();
    last->tick   = current_tick;

    /* Produce keyclick */
    keyclick_click(false, action);

    return action;
}

/********************************************************
* init_act_cur initializes passed struct action_cur_t
* with context, timeout,and get_context_map.
* other values set to default
* if get_context_map is NULL standard
* context mapping will be used
*/
static void init_act_cur(action_cur_t *cur,
                         int  context, int  timeout,
                         const struct button_mapping* (*get_context_map)(int))
{
    cur->action              = ACTION_UNKNOWN;
    cur->button              = BUTTON_NONE;
    cur->context             = context;
    cur->is_prebutton        = false;
    cur->items               = NULL;
    cur->timeout             = timeout;
    cur->get_context_map = get_context_map;
}

/*******************************************************
* do_backlight allows exemptions to the backlight on
* user selected actions; Actions need to be looked up
* before the decision to turn on backlight is made,
* if selective backlighting is enabled then
* filter first keypress events may need
* to be taken into account as well
* IF SEL_ACTION_ENABLED then:
* Returns action or is FFKeypress is enabled,
* ACTION_NONE on first keypress
* delays backlight_on until action is known
* handles backlight_on if needed
*/
static inline int do_backlight(action_last_t *last, action_cur_t *cur, int action)
{

    if (!has_flag(last->backlight_mask, SEL_ACTION_ENABLED)
        || (action & (SYS_EVENT|BUTTON_MULTIMEDIA)) != 0
        || action == ACTION_REDRAW)
    {
        return action;
    }

    bool filtered;
    bool bl_activate = false;
    bool bl_is_off = !is_backlight_on(false);

    bl_is_off &= !(has_flag(last->backlight_mask, SEL_ACTION_NOEXT)
                     && power_input_present());
    /* skip if backlight on | incorrect context | SEL_ACTION_NOEXT + ext pwr */
    if (bl_is_off && (cur->context == CONTEXT_WPS ||
       cur->context == CONTEXT_MAINMENU))
    {
        filtered = is_action_filtered(action, last->backlight_mask, cur->context);
        bl_activate = !is_action_discarded(cur, filtered, &last->bl_filter_tick);
    }
    else /* standard backlight behaviour */
    {
        bl_activate = true;
    }

    if (action != ACTION_NONE && bl_activate)
    {
        action_handle_backlight(true, true);
        /* Handle first keypress enables backlight only */
        if (has_flag(last->backlight_mask, SEL_ACTION_FFKEYPRESS) && bl_is_off)
        {
            action       = ACTION_NONE;
            last->button = BUTTON_NONE;
        }
    }
    else
    {
        action_handle_backlight(false, true);/* set ignore next true */
    }

    return action;
}

/********************************************************************
* get_action_worker() searches the button list of the passed context
* for the just pressed button. If there is a match it returns the
* value from the list. If there is no match, the last item in the
* list "points" to the next context in a chain so the "chain" is
* followed until the button is found. ACTION_NONE int the button
* list will get CONTEXT_STD which is always the last list checked.
*
* BE AWARE IF YOUR DESIRED ACTION IS IN A LOWER 'CHAINED' CONTEXT::
* *** is_prebutton can miss pre_buttons
* ** An action without pre_button_code (pre_button_code = BUTTON_NONE)
* *  will be returned from the higher context see action_code_worker()
*  for a more in-depth explanation
*
*   Timeout can be: TIMEOUT_NOBLOCK to return immediatly
*                   TIMEOUT_BLOCK   to wait for a button press
*                   Any number >0   to wait that many ticks for a press
*/
static int get_action_worker(action_last_t *last, action_cur_t *cur)
{
    send_event(GUI_EVENT_ACTIONUPDATE, NULL);

    /*if button = none/special; returns immediately*/
    if (action_poll_button(last, cur))
    {
        return cur->button;
    }

    update_screen_has_lock(last, cur);

    if (get_action_touchscreen(last, cur))
    {
        do_softlock(last, cur);
        return cur->action;
    }

    button_flip_horizontally(cur->context, &cur->button);

    action_code_lookup(last, cur);

    do_softlock(last, cur);

    return update_action_last(last, cur);
}
/*
*******************************************************************************
* END INTERNAL ACTION FUNCTIONS ***********************************************
*******************************************************************************/

/******************************************************************************
* EXPORTED ACTION FUNCTIONS ***************************************************
*******************************************************************************
*/

bool action_userabort(int timeout)
{
    /* This is the "poll for cancel during a long operation" call, used while a
     * progress splash is on screen (database build, file ops, playlist build,
     * ...). get_custom_action fires GUI_EVENT_ACTIONUPDATE, which re-renders
     * and flushes the status-bar skin over the splash -> flicker. Inhibit the
     * flush across the poll so the splash keeps the screen. */
    skin_inhibit_flush(true);
    int  action = get_custom_action(CONTEXT_STD, timeout, NULL);
    skin_inhibit_flush(false);
    bool ret    = (action == ACTION_STD_CANCEL);
    if (!ret)
    {
        default_event_handler(action);
    }

    return ret;
}

void action_wait_for_release(void)
{
    if (!(action_last.button & BUTTON_REL))
        action_last.wait_for_release = true;
    button_clear_pressed();
}

int get_action(int context, int timeout)
{
    action_cur_t current;
    init_act_cur(&current, context, timeout, NULL);

    int action = get_action_worker(&action_last, &current);


    action = do_backlight(&action_last, &current, action);

    return action;
}

int action_set_keymap(struct button_mapping* core_keymap, int count)
{
#ifdef DISABLE_ACTION_REMAP
    (void)core_keymap;
    (void)count;
    return -1;
#else
    if (count <= 0 || core_keymap == NULL)
        return action_set_keymap_handle(0, 0);

    size_t keyremap_buf_size = count * sizeof(struct button_mapping);
    int handle = core_alloc(keyremap_buf_size);
    if (handle < 0)
        return -6;

    memcpy(core_get_data(handle), core_keymap, keyremap_buf_size);
    return action_set_keymap_handle(handle, count);
#endif
}

int action_set_keymap_handle(int handle, int count)
{
#ifdef DISABLE_ACTION_REMAP
    (void)core_keymap;
    (void)count;
    return -1;
#else
    /* free an existing remap */
    action_last.key_remap = core_free(action_last.key_remap);

    /* if clearing the remap, we're done */
    if (count <= 0 || handle <= 0)
        return 0;

    /* validate the keymap */
    struct button_mapping* core_keymap = core_get_data(handle);
    struct button_mapping* entry = &core_keymap[count - 1];
    if (entry->action_code != (int) CONTEXT_STOPSEARCHING ||
        entry->button_code != BUTTON_NONE) /* check for sentinel at end*/
    {
        /* missing sentinel entry */
        return -1;
    }

    /* check the lut at the beginning for invalid offsets */
    for (int i = 0; i < count; ++i)
    {
        entry = &core_keymap[i];
        if (entry->action_code == (int)CONTEXT_STOPSEARCHING)
            break;

        if ((entry->action_code & CONTEXT_REMAPPED) == CONTEXT_REMAPPED)
        {
            int firstbtn = entry->button_code;
            int endpos = firstbtn + entry->pre_button_code;
            if (firstbtn > count || firstbtn < i || endpos > count)
            {
                /* offset out of bounds */
                return -2;
            }

            if (core_keymap[endpos].button_code != BUTTON_NONE)
            {
                /* stop sentinel is not at end of action lut */
                return -3;
            }
        }
        else
        {
            /* something other than a context remap in the lut */
            return -4;
        }

        if (i+1 >= count)
        {
            /* no sentinel in the lut */
            return -5;
        }
    }

    /* success */
    action_last.key_remap = handle;
    return count;
#endif
}

int get_custom_action(int context,int timeout,
                      const struct button_mapping* (*get_context_map)(int))
{
    action_cur_t current;
    init_act_cur(&current, context, timeout, get_context_map);

    int action = get_action_worker(&action_last, &current);

    action = do_backlight(&action_last, &current, action);

    return action;
}

intptr_t get_action_data(void)
{
    return action_last.data;
}

int get_action_statuscode(int *button)
{
    int ret = 0;
    if (button)
    {
        *button = action_last.button;
    }

    if (has_flag(action_last.button, BUTTON_REMOTE))
    {
        ret |= ACTION_REMOTE;
    }

    if (action_last.repeated)
    {
        ret |= ACTION_REPEAT;
    }

    return ret;
}

/* Enable selected actions to leave the backlight off */
void set_selective_backlight_actions(bool selective, unsigned int mask,
                                                              bool filter_fkp)
{
    action_handle_backlight(true, selective);
    if (selective) /* we will handle filter_first_keypress here so turn it off*/
    {
        set_backlight_filter_keypress(false);/* turnoff ffkp in button.c */
        action_last.backlight_mask = mask | SEL_ACTION_ENABLED;
        if (filter_fkp)
        {
            action_last.backlight_mask |= SEL_ACTION_FFKEYPRESS;
        }
    }
    else
    {
        set_backlight_filter_keypress(filter_fkp);
        action_last.backlight_mask = SEL_ACTION_NONE;
    }
}


/*
*******************************************************************************
* END EXPORTED ACTION FUNCTIONS ***********************************************
*******************************************************************************/
