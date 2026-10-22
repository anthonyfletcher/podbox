/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to eq_editor.c.
 ****************************************************************************/
#ifndef _EQ_EDITOR_H
#define _EQ_EDITOR_H

/* global_settings.eq_editor_mode */
enum {
    EQ_EDITOR_FOLLOW = 0,       /* Simple at Standard, Enhanced at Everything */
    EQ_EDITOR_SIMPLE,
    EQ_EDITOR_ENHANCED,
};

/* The curve editor. MENU_ATTACHED_USB if USB took the screen. */
int eq_editor_screen(void);

/* The built-in presets, then the .cfg files in /.rockbox/eqs. Moving the
 * selection plays a built-in preset at once; Select keeps the choice and
 * Menu puts back what was there. MENU_ATTACHED_USB as above. */
int eq_presets_screen(void);

#endif
