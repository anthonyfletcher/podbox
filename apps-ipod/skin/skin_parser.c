/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/skin_engine/skin_parser.c
 * Copyright (C) 2007 Nicolas Pennequin, Dan Everton, Matthias Mohr
 * Portions Copyright (C) 2026 RockPod contributors
 *               2010 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Parses a .wps/.sbs skin file into the internal element tree: tags,
 * viewports, conditionals, images and their buflib allocations.
 *
 * The whole parsed tree lives inside one movable buflib allocation, which is
 * why nothing in it is linked by pointer. Every reference is an *offset* from
 * the base of that block, declared OFFSETTYPE(x) and dereferenced through
 * SKINOFFSETTOPTR(skin_buffer, off) -- see lib/skin_parser/skin_parser.h.
 * Offsets survive the block being moved; pointers would not. A negative
 * offset means NULL. Storing a raw pointer into the tree is the mistake to
 * watch for here.
 *
 * Parsing is table-driven: each skin tag names a parse function in the tag
 * table, and adding a tag means adding an entry plus its parse_*() below.
 *
 * Parts, in order:
 *   - parameter accessors and the offset-linked token lists
 *   - image, font and album-art loading (the tags that own resources)
 *   - viewport tags: colours, gradients, text style, rectangles
 *   - conditionals, logical operators and timeouts
 *   - progress bars and the list/spectrum display tags
 *   - the tag table, and skin_parse() driving the whole thing
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "system/hash.h"
#include "core_alloc.h"
#include "file.h"
#include "kernel.h"
#include "system/app_util.h"
#include "draw/color.h"
#include "system/strutil.h"
#include "system/app_buffer.h"
#include "draw/viewport.h"
#include "draw/round_rect.h"
#include "audio/spectrum_meter.h"   /* SPECTRUM_MAX_BANDS */

#include "skin_buffer.h"
#include "skin_debug.h"
#include "skin_parser.h"
#include "tag_table.h"

#include "debug.h"
#include "widgets/splash.h"
#include "lang.h"
#include "speech/language.h"

#include <ctype.h>
#include <stdbool.h>
#include "font.h"

#include "wps_internals.h"
#include "custom_tokens.h"
#include "skin_engine.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "rbpaths.h"
#include "pathfuncs.h"
#include "draw/bmp.h"
#include "draw/img_filter.h"

#include "audio/playback.h"

#include "backdrop.h"
#include "statusbar_skinned.h"
#include "string-extra.h"

#define WPS_ERROR_INVALID_PARAM         -1

static char* skin_buffer = NULL;
static char *backdrop_filename;
static struct skin_stats *_stats = NULL;

static bool isdefault(struct skin_tag_parameter *param)
{
    return param->type == DEFAULT;
}

static inline char*
get_param_text(struct skin_element *element, int param_number)
{
    struct skin_tag_parameter* params = SKINOFFSETTOPTR(skin_buffer, element->params);
    return SKINOFFSETTOPTR(skin_buffer, params[param_number].data.text);
}

static inline struct skin_element*
get_param_code(struct skin_element *element, int param_number)
{
    struct skin_tag_parameter* params = SKINOFFSETTOPTR(skin_buffer, element->params);
    return SKINOFFSETTOPTR(skin_buffer, params[param_number].data.code);
}

static inline struct skin_tag_parameter*
get_param(struct skin_element *element, int param_number)
{
    struct skin_tag_parameter* params = SKINOFFSETTOPTR(skin_buffer, element->params);
    return &params[param_number];
}

/* which screen are we parsing for? */
static enum screen_type curr_screen;

/* the current viewport */
static struct skin_element *curr_viewport_element;
static struct skin_viewport *curr_vp;
static struct skin_element *first_viewport;

static struct line *curr_line;

/* The %Cl parsed most recently, which is the art a bare %Cd draws. NULL until
 * the first one, so a %Cd written above every %Cl still resolves -- at render
 * time, to the skin's first. */
static struct skin_albumart *curr_albumart;

static int follow_lang_direction = 0;

typedef int (*parse_function)(struct skin_element *element,
                              struct wps_token *token,
                              struct wps_data *wps_data);

/* add a skin_token_list item to the list chain. ALWAYS appended because some of the
 * chains require the order to be kept.
 */
static void add_to_ll_chain(OFFSETTYPE(struct skin_token_list *) *listoffset,
        struct skin_token_list *item)
{
    struct skin_token_list *list = SKINOFFSETTOPTR(skin_buffer, *listoffset);
    if (list == NULL)
    {
        *listoffset = PTRTOSKINOFFSET(skin_buffer, item);
    }
    else
    {
        while (SKINOFFSETTOPTR(skin_buffer, list->next))
            list = SKINOFFSETTOPTR(skin_buffer, list->next);
        list->next = PTRTOSKINOFFSET(skin_buffer, item);
    }
}

void *skin_find_item(const char *label, enum skin_find_what what,
                     struct wps_data *data)
{
    char *databuf = get_skin_buffer(data);
    union {
        struct skin_token_list *linkedlist;
        struct skin_element *vplist;
    } list = {NULL};
    bool isvplist = false;
    void *ret = NULL;
    if (!databuf)
        databuf = skin_buffer;
    switch (what)
    {
        case SKIN_FIND_UIVP:
        case SKIN_FIND_VP:
            list.vplist = SKINOFFSETTOPTR(databuf, data->tree);
            isvplist = true;
        break;
        case SKIN_FIND_IMAGE:
            list.linkedlist = SKINOFFSETTOPTR(databuf, data->images);
        break;
        case SKIN_FIND_ALBUMART:
            list.linkedlist = SKINOFFSETTOPTR(databuf, data->albumart);
        break;
        case SKIN_VARIABLE:
            list.linkedlist = SKINOFFSETTOPTR(databuf, data->skinvars);
        break;
    }

    while (list.linkedlist)
    {
        bool skip = false;
        struct wps_token *token = NULL;
        const char *itemlabel = NULL;
        if (!isvplist)
            token = SKINOFFSETTOPTR(databuf, list.linkedlist->token);
        switch (what)
        {
            case SKIN_FIND_UIVP:
            case SKIN_FIND_VP:
                ret = SKINOFFSETTOPTR(databuf, list.vplist->data);
                if (!ret) break;
                if (((struct skin_viewport *)ret)->label == VP_DEFAULT_LABEL)
                    itemlabel = VP_DEFAULT_LABEL_STRING;
                else
                    itemlabel = SKINOFFSETTOPTR(databuf, ((struct skin_viewport *)ret)->label);
                skip = !(((struct skin_viewport *)ret)->is_infovp ==
                    (what==SKIN_FIND_UIVP));
                break;
            case SKIN_FIND_IMAGE:
                if (!token) break;
                ret = SKINOFFSETTOPTR(databuf, token->value.data);
                if (!ret) break;
                itemlabel = SKINOFFSETTOPTR(databuf, ((struct gui_img *)ret)->label);
                break;
            case SKIN_FIND_ALBUMART:
                if (!token) break;
                ret = SKINOFFSETTOPTR(databuf, token->value.data);
                if (!ret) break;
                itemlabel = SKINOFFSETTOPTR(databuf,
                                        ((struct skin_albumart *)ret)->label);
                break;
            case SKIN_VARIABLE:
                if (!token) break;
                ret = SKINOFFSETTOPTR(databuf, token->value.data);
                if (!ret) break;
                itemlabel = SKINOFFSETTOPTR(databuf, ((struct skin_var *)ret)->label);
                break;
        }
        if (!skip && itemlabel && !strcmp(itemlabel, label))
        {
            return ret;
        }

        if (isvplist)
            list.vplist = SKINOFFSETTOPTR(databuf, list.vplist->next);
        else
            list.linkedlist = SKINOFFSETTOPTR(databuf, list.linkedlist->next);
    }
    return NULL;
}

/* create and init a new wpsll item.
 * passing NULL to token will alloc a new one.
 * You should only pass NULL for the token when the token type (table above)
 * is WPS_NO_TOKEN which means it is not stored automatically in the skins token array
 */
static struct skin_token_list *new_skin_token_list_item(struct wps_token *token,
                                                        void* token_data)
{
    struct skin_token_list *llitem = skin_buffer_alloc(sizeof(*llitem));
    if (!token)
        token = skin_buffer_alloc(sizeof(*token));
    if (!llitem || !token)
        return NULL;
    llitem->next = PTRTOSKINOFFSET(skin_buffer, NULL);
    llitem->token = PTRTOSKINOFFSET(skin_buffer, token);
    if (token_data)
        token->value.data = PTRTOSKINOFFSET(skin_buffer, token_data);
    return llitem;
}

static int parse_statusbar_tags(struct skin_element* element,
                                struct wps_token *token,
                                struct wps_data *wps_data)
{
    (void)element;
    if (token->type == SKIN_TOKEN_DRAW_INBUILTBAR)
    {
        token->value.data = PTRTOSKINOFFSET(skin_buffer, (void*)&curr_vp->vp);
    }
    else
    {
        struct skin_viewport *skin_default = SKINOFFSETTOPTR(skin_buffer, first_viewport->data);
        if (first_viewport->params_count == 0)
        {
            wps_data->wps_sb_tag = true;
            wps_data->show_sb_on_wps = (token->type == SKIN_TOKEN_ENABLE_THEME);
        }
        if (wps_data->show_sb_on_wps)
        {
            viewport_set_defaults(&skin_default->vp, curr_screen);
        }
        else
        {
            viewport_set_fullscreen(&skin_default->vp, curr_screen);
        }
    }
    return 0;
}

static int get_image_id(int c)
{
    if(c >= 'a' && c <= 'z')
        return c - 'a';
    else if(c >= 'A' && c <= 'Z')
        return c - 'A' + 26;
    else
        return -1;
}

void get_image_filename(const char *start, const char* bmpdir,
                                char *buf, int buf_size)
{
    path_append(buf, bmpdir, start, buf_size);
}

static int parse_image_display(struct skin_element *element,
                               struct wps_token *token,
                               struct wps_data *wps_data)
{
    char *label = get_param_text(element, 0);
    char sublabel = '\0';
    int subimage;
    struct gui_img *img;
    struct image_display *id = skin_buffer_alloc(sizeof(*id));

    if (element->params_count == 1 && strlen(label) <= 2)
    {
        /* backwards compatability. Allow %xd(Aa) to still work */
        sublabel = label[1];
        label[1] = '\0';
    }
    /* sanity check */
    img = skin_find_item(label, SKIN_FIND_IMAGE, wps_data);
    if (!img || !id)
    {
        return WPS_ERROR_INVALID_PARAM;
    }
    id->label = img->label;
    id->offset = 0;
    id->token = PTRTOSKINOFFSET(skin_buffer, NULL);
    if (img->using_preloaded_icons)
    {
        token->type = SKIN_TOKEN_IMAGE_DISPLAY_LISTICON;
    }

    if (token->type == SKIN_TOKEN_IMAGE_DISPLAY_9SEGMENT)
        img->is_9_segment = true;

    if (element->params_count > 1)
    {
        struct skin_tag_parameter *param1 = get_param(element, 1);
        if (param1->type == CODE)
            id->token = get_param_code(element, 1)->data;
        /* specify a number. 1 being the first subimage (i.e top) NOT 0 */
        else if (param1->type == INTEGER)
            id->subimage = param1->data.number - 1;
        if (element->params_count > 2)
            id->offset = get_param(element, 2)->data.number;
    }
    else
    {
        if ((subimage = get_image_id(sublabel)) != -1)
        {
            if (subimage >= img->num_subimages)
                return WPS_ERROR_INVALID_PARAM;
            id->subimage = subimage;
        } else {
            id->subimage = 0;
        }
    }
    token->value.data = PTRTOSKINOFFSET(skin_buffer, id);
    return 0;
}

/* helper function if param type is PERCENT multiply by mult and divide by 1000 */
static int percent_parse_param(struct skin_tag_parameter *param, int mult)
{
    if (param->type == PERCENT)
        return param->data.number * mult / 1000;
    return param->data.number;
}

static int parse_image_load(struct skin_element *element,
                            struct wps_token *token,
                            struct wps_data *wps_data)
{
    const char* filename;
    const char* id;
    int x = 0,y = 0, subimages = 1;
    struct gui_img *img;

    /* format: %x(n,filename.bmp[,x,y])
       or %xl(n,filename.bmp[,x,y])
       or %xl(n,filename.bmp[,x,y,num_subimages])
    */

    id = get_param_text(element, 0);
    filename = get_param_text(element, 1);
    /* x,y,num_subimages handling:
     * If all 3 are left out use sane defaults.
     * If there are 2 params it must be x,y
     * if there is only 1 param it must be the num_subimages
     */
    if (element->params_count == 3)
        subimages = get_param(element, 2)->data.number;
    else if (element->params_count > 3)
    {
        x = percent_parse_param(get_param(element, 2), curr_vp->vp.width);
        y = percent_parse_param(get_param(element, 3), curr_vp->vp.height);

        if (element->params_count == 5)
            subimages = get_param(element, 4)->data.number;
    }
    /* check the image number and load state */
    if(skin_find_item(id, SKIN_FIND_IMAGE, wps_data))
    {
        /* Invalid image ID */
        return WPS_ERROR_INVALID_PARAM;
    }
    img = skin_buffer_alloc(sizeof(*img));
    if (!img)
        return WPS_ERROR_INVALID_PARAM;
    /* save a pointer to the filename */
    img->bm.data = (char*)filename;
    img->label = PTRTOSKINOFFSET(skin_buffer, (void*)id);
    img->x = x;
    img->y = y;
    img->num_subimages = subimages;
    img->display = -1;
    img->using_preloaded_icons = false;
    img->buflib_handle = -1;
    img->is_9_segment = false;
    img->loaded = false;
    img->dither = false;

    if (token->type == SKIN_TOKEN_IMAGE_DISPLAY)
    {
        token->value.data = PTRTOSKINOFFSET(skin_buffer, img);
        if (curr_vp)
            img->dither = curr_vp->output_to_backdrop_buffer;
    }

    if (!strcmp(img->bm.data, "__list_icons__"))
    {
        img->num_subimages = Icon_Last_Themeable;
        img->using_preloaded_icons = true;
    }

    struct skin_token_list *item = new_skin_token_list_item(NULL, img);
    if (!item)
        return WPS_ERROR_INVALID_PARAM;
    add_to_ll_chain(&wps_data->images, item);

    return 0;
}
struct skin_font {
    int id; /* the id from font_load */
    char *name;  /* filename without path and extension */
    int glyphs;  /* how many glyphs to reserve room for */
};
static struct skin_font skinfonts[MAXUSERFONTS];
static int parse_font_load(struct skin_element *element,
                           struct wps_token *token,
                           struct wps_data *wps_data)
{
    (void)wps_data; (void)token;
    int id = get_param(element, 0)->data.number;
    char *filename = get_param_text(element, 1);
    int  glyphs;
    char *ptr;

    if(element->params_count > 2)
        glyphs = get_param(element, 2)->data.number;
    else
        glyphs = global_settings.glyphs_to_cache;
    if (id < 2)
    {
        DEBUGF("font id must be >= 2 (%d)\n", id);
        return -1;
    }
#if defined(DEBUG)
    if (skinfonts[id-2].name != NULL)
    {
        DEBUGF("font id %d already being used\n", id);
    }
#endif
    /* make sure the filename contains .fnt,
     * we dont actually use it, but require it anyway */
    ptr = strrchr(filename, '.');
    if (!ptr || strncmp(ptr, ".fnt", 4))
        return WPS_ERROR_INVALID_PARAM;
    skinfonts[id-2].id = -1;
    skinfonts[id-2].name = filename;
    skinfonts[id-2].glyphs = glyphs;

    return 0;
}

static int parse_playlistview(struct skin_element *element,
                              struct wps_token *token,
                              struct wps_data *wps_data)
{
    (void)wps_data;
    struct playlistviewer *viewer = skin_buffer_alloc(sizeof(*viewer));
    if (!viewer)
        return WPS_ERROR_INVALID_PARAM;
    viewer->show_icons = true;
    viewer->start_offset = get_param(element, 0)->data.number;
    viewer->line = PTRTOSKINOFFSET(skin_buffer, get_param_code(element, 1));

    token->value.data = PTRTOSKINOFFSET(skin_buffer, (void*)viewer);

    return 0;
}

static int parse_viewport_gradient_setup(struct skin_element *element,
                                   struct wps_token *token,
                                   struct wps_data *wps_data)
{
    (void)wps_data;
    struct gradient_config *cfg;
    if (element->params_count < 2) /* only start and end are required */
        return 1;
    cfg = skin_buffer_alloc(sizeof(*cfg));
    if (!cfg)
        return 1;
    if (!parse_color(curr_screen, get_param_text(element, 0), &cfg->start) ||
        !parse_color(curr_screen, get_param_text(element, 1), &cfg->end))
        return 1;
    if (element->params_count > 2)
    {
        if (!parse_color(curr_screen, get_param_text(element, 2), &cfg->text))
            return 1;
    }
    else
    {
        cfg->text = curr_vp->vp.fg_pattern;
    }

    token->value.data = PTRTOSKINOFFSET(skin_buffer, cfg);
    return 0;
}

static int parse_listitem(struct skin_element *element,
                        struct wps_token *token,
                        struct wps_data *wps_data)
{
    (void)wps_data;
    struct listitem *li = skin_buffer_alloc(sizeof(*li));
    if (!li)
        return -1;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, li);
    li->radius = 0;
    li->mask = PTRTOSKINOFFSET(skin_buffer, NULL);
    li->filter = PTRTOSKINOFFSET(skin_buffer, NULL);
    li->filter_hash = 0;
    if (element->params_count == 0)
        li->offset = 0;
    else
    {
        li->offset = get_param(element, 0)->data.number;
        /* Written as '-' to reach the radius past it, wrap stays on -- and the
         * text of a defaulted parameter must not be read at all. */
        if (element->params_count > 1 && !isdefault(get_param(element, 1)))
            li->wrap = strcasecmp(get_param_text(element, 1), "nowrap") != 0;
        else
            li->wrap = true;
    }

    /* Third parameter: corner radius. Only %La's argument list reaches this
     * far -- the text and icon tags sharing this parser take two. */
    if (element->params_count > 2 && !isdefault(get_param(element, 2)))
    {
        int radius = get_param(element, 2)->data.number;
        unsigned char *mask;

        if (radius < 0 || radius > ROUND_RECT_MAX_RADIUS)
            return -1;
        if (radius > 0)
        {
            mask = skin_buffer_alloc(round_rect_mask_size(radius));
            if (!mask)
                return -1;
            round_rect_build_mask(mask, radius, LCD_BLEND_OPAQUE);
            li->radius = (uint8_t)radius;
            li->mask = PTRTOSKINOFFSET(skin_buffer, mask);
        }
    }

    /* Fourth parameter: the filter chain, compiled here so a misspelt filter
     * fails the skin at load rather than quietly drawing nothing.
     *
     * IMG_CLASSES_INPLACE refuses two things. `blur` needs a destination of
     * its own, and a row's cover is drawn from a browser slot sized for the
     * picture and nothing else. The adaptive filters need the cover measured
     * before the chain runs, and nothing on this path measures one -- so they
     * are refused here too, rather than compiling into a pass that costs a
     * walk over every thumbnail and changes nothing. */
    if (element->params_count > 3 && !isdefault(get_param(element, 3)))
    {
        struct img_filter *f = skin_buffer_alloc(sizeof(*f));

        if (!f)
            return -1;
        if (!img_filter_compile(get_param_text(element, 3),
                                IMG_CLASSES_INPLACE, f))
        {
            DEBUGF("%%La: %s\n", f->error);
            return -1;
        }
        if (f->stages)
        {
            li->filter = PTRTOSKINOFFSET(skin_buffer, f);
            /* FNV-1a over the compiled chain: two %La naming the same
             * treatment share a slot, and a slot loaded under one chain is
             * never reused for another. Everything up to `error`, which is
             * the one field that says nothing about what the chain does.
             * img_filter_compile() zeroes the whole struct first, so the
             * padding between fields is hashed and is stable. */
            uint32_t h = fnv1a_bytes(f, offsetof(struct img_filter, error));

            li->filter_hash = h ? h : 1;   /* 0 means "no chain" */
        }
    }
    return 0;
}

static int parse_spectrumbars(struct skin_element *element,
                              struct wps_token *token,
                              struct wps_data *wps_data)
{
    (void)wps_data;
    struct spectrum_bars *sb = skin_buffer_alloc(sizeof(*sb));
    if (!sb)
        return -1;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, sb);
    sb->bars = get_param(element, 0)->data.number;
    if (sb->bars < 1)
        sb->bars = 1;
    else if (sb->bars > SPECTRUM_MAX_BANDS)
        sb->bars = SPECTRUM_MAX_BANDS;
    /* skin_buffer_alloc() does not zero, so both of these are written on
     * every path. Radiate opens out from the centre of the viewport in both
     * directions, so it carries centre growth with it. */
    sb->center_aligned = false;
    sb->radiate = false;
    if (element->params_count > 1 && !isdefault(get_param(element, 1)))
    {
        const char *align = get_param_text(element, 1);
        if (strcasecmp(align, "radiate") == 0)
            sb->center_aligned = sb->radiate = true;
        else
            sb->center_aligned = strcasecmp(align, "center") == 0;
    }
    /* Corner radius. No upper clamp: fill_round_rect() fits it to whatever
     * bar it is drawing, and a short bar rounds less than a tall one. */
    if (element->params_count > 2 && !isdefault(get_param(element, 2)))
        sb->radius = get_param(element, 2)->data.number;
    else
        sb->radius = 0;
    if (sb->radius < 0)
        sb->radius = 0;
    /* Gap between bars. Absent keeps what every %Sb written before this got:
     * one pixel, or none when there is only one bar to separate. */
    if (element->params_count > 3 && !isdefault(get_param(element, 3)))
        sb->gap = get_param(element, 3)->data.number;
    else
        sb->gap = (sb->bars > 1 || sb->radiate) ? 1 : 0;
    if (sb->gap < 0)
        sb->gap = 0;
    return 0;
}

static int parse_listitemviewport(struct skin_element *element,
                                  struct wps_token *token,
                                  struct wps_data *wps_data)
{
    struct skin_tag_parameter *param;
    struct listitem_viewport_cfg *cfg = skin_buffer_alloc(sizeof(*cfg));
    if (!cfg)
        return -1;
    cfg->data = wps_data;
    /* skin_buffer_alloc() does not zero, and both of these are read on every
     * row draw whether or not the tag named them. */
    cfg->tile = false;
    cfg->art_height = 0;
    cfg->label = PTRTOSKINOFFSET(skin_buffer, get_param_text(element, 0));
    cfg->width = -1;
    cfg->height = -1;

    param = get_param(element, 1);
    if (!isdefault(param))
        cfg->width = param->data.number;

    param = get_param(element, 2);
    if (!isdefault(param))
        cfg->height = param->data.number;

    /* isdefault() first: the slot is a nullable string now, so '-' can appear
     * here to reach the art height past it, and the text of a defaulted
     * parameter must not be read at all. */
    if (element->params_count > 3 && !isdefault(get_param(element, 3)) &&
        !strcmp(get_param_text(element, 3), "tile"))
        cfg->tile = true;

    /* The row height for a list that draws art. Zero means the theme says
     * nothing and the setting still answers -- see list_item_height(). */
    if (element->params_count > 4 && !isdefault(get_param(element, 4)))
        cfg->art_height = get_param(element, 4)->data.number;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, (void*)cfg);
    return 0;
}

static int parse_viewporttextstyle(struct skin_element *element,
                                   struct wps_token *token,
                                   struct wps_data *wps_data)
{
    (void)wps_data;
    char *mode = get_param_text(element, 0);
    struct line_desc *line = skin_buffer_alloc(sizeof(*line));
    *line = (struct line_desc)LINE_DESC_DEFINIT;
    unsigned colour;

    static const char * const vp_options[] = { "invert", "color", "colour",
                                 "clear", "gradient", NULL};

    int vp_op = string_option(mode, vp_options, false);

    if (vp_op == 0) /*invert*/
    {
        line->style = STYLE_INVERT;
    }
    else if (vp_op == 1 || vp_op == 2) /*color/colour*/
    {
        if (element->params_count < 2 ||
            !parse_color(curr_screen, get_param_text(element, 1), &colour))
            return 1;
        /* STYLE_COLORED is only a modifier and can't be used on its own */
        line->style = STYLE_COLORED | STYLE_DEFAULT;
        line->text_color = colour;
    }
    else if (vp_op == 4) /*gradient*/
    {
        int num_lines;
        if (element->params_count < 2)
            num_lines = 1;
        else /* atoi() instead of using a number in the parser is because [si]
              * will select the number for something which looks like a colour
              * making the "colour" case (above) harder to parse */
            num_lines = atoi(get_param_text(element, 1));
        line->style = STYLE_GRADIENT;
        line->nlines = num_lines;
    }
    else if (vp_op == 3) /*clear*/
    {
        line->style = STYLE_DEFAULT;
    }
    else
        return 1;

    token->value.data = PTRTOSKINOFFSET(skin_buffer, line);
    return 0;
}

static int parse_drawrectangle( struct skin_element *element,
                                struct wps_token *token,
                                struct wps_data *wps_data)
{
    (void)wps_data;
    struct skin_tag_parameter *param;
    struct draw_rectangle *rect = skin_buffer_alloc(sizeof(*rect));

    if (!rect)
        return -1;

    rect->x = percent_parse_param(get_param(element, 0), curr_vp->vp.width);
    rect->y = percent_parse_param(get_param(element, 1), curr_vp->vp.height);

    param = get_param(element, 2);
    if (isdefault(param))
        rect->width = curr_vp->vp.width - rect->x;
    else
        rect->width = percent_parse_param(param, curr_vp->vp.width);

    param = get_param(element, 3);
    if (isdefault(param))
        rect->height = curr_vp->vp.height - rect->y;
    else
        rect->height = percent_parse_param(param, curr_vp->vp.height);

    rect->start_colour = curr_vp->vp.fg_pattern;
    rect->end_colour = curr_vp->vp.fg_pattern;

    /* The colours accept '-' as well as being absent, so an opacity can be
     * given without one -- which is what a panel tinted in the viewport's own
     * (possibly album-derived) foreground needs. */
    if (element->params_count > 4 && !isdefault(get_param(element, 4)))
    {
        if (!parse_color(curr_screen, get_param_text(element, 4),
                    &rect->start_colour))
            return -1;
        rect->end_colour = rect->start_colour;
    }
    if (element->params_count > 5 && !isdefault(get_param(element, 5)))
    {
        if (!parse_color(curr_screen, get_param_text(element, 5),
                    &rect->end_colour))
            return -1;
    }

    /* Seventh parameter: opacity, 0..LCD_BLEND_OPAQUE. Absent means opaque, so
     * every %dr written before this existed keeps the plain fill path. */
    rect->opacity = LCD_BLEND_OPAQUE;
    if (element->params_count > 6)
    {
        param = get_param(element, 6);
        if (!isdefault(param))
        {
            if (param->data.number < 0 ||
                param->data.number > LCD_BLEND_OPAQUE)
                return -1;
            rect->opacity = (uint8_t)param->data.number;
        }
    }

    /* Eighth parameter: corner radius. The coverage mask belongs to the tag
     * rather than to a cache, because both of the things it depends on are
     * settled here -- two rounded rectangles of different radii would
     * otherwise rebuild it against each other on every frame they share. */
    rect->radius = 0;
    rect->mask = PTRTOSKINOFFSET(skin_buffer, NULL);
    if (element->params_count > 7 && !isdefault(get_param(element, 7)))
    {
        int radius = get_param(element, 7)->data.number;
        unsigned char *mask;

        if (radius < 0 || radius > ROUND_RECT_MAX_RADIUS)
            return -1;
        /* A radius larger than the box is reduced to fit, the way the
         * hard-edged rounded rectangles treat one. */
        if (radius > rect->width / 2)
            radius = rect->width / 2;
        if (radius > rect->height / 2)
            radius = rect->height / 2;

        if (radius > 0)
        {
            mask = skin_buffer_alloc(round_rect_mask_size(radius));
            if (!mask)
                return -1;
            round_rect_build_mask(mask, radius, rect->opacity);
            rect->radius = (uint8_t)radius;
            rect->mask = PTRTOSKINOFFSET(skin_buffer, mask);
        }
    }

    token->value.data = PTRTOSKINOFFSET(skin_buffer, rect);

    return 0;
}

static int parse_viewportcolour(struct skin_element *element,
                                struct wps_token *token,
                                struct wps_data *wps_data)
{
    (void)wps_data;
    struct skin_tag_parameter *param = get_param(element, 0);
    struct viewport_colour *colour = skin_buffer_alloc(sizeof(*colour));
    if (!colour)
        return -1;
    if (isdefault(param))
    {
        unsigned int fg_color;
        unsigned int bg_color;

        switch (curr_screen)
        {
        default:
            fg_color = global_settings.fg_color;
            bg_color = global_settings.bg_color;
            break;
        }

        if (token->type == SKIN_TOKEN_VIEWPORT_FGCOLOUR)
            colour->colour = fg_color;
        else
            colour->colour = bg_color;
        colour->is_default = true;
    }
    else
    {
        if (!parse_color(curr_screen, SKINOFFSETTOPTR(skin_buffer, param->data.text),
                    &colour->colour))
            return -1;
        colour->is_default = false;
    }
    token->value.data = PTRTOSKINOFFSET(skin_buffer, colour);
    if (element->line == curr_viewport_element->line)
    {
        if (token->type == SKIN_TOKEN_VIEWPORT_FGCOLOUR)
        {
            curr_vp->vp.fg_pattern = colour->colour;
            curr_vp->dc_orig_fg = colour->colour;
        }
        else
        {
            curr_vp->vp.bg_pattern = colour->colour;
            curr_vp->dc_orig_bg = colour->colour;
        }
    }
    return 0;
}

static int parse_image_special(struct skin_element *element,
                               struct wps_token *token,
                               struct wps_data *wps_data)
{
    (void)wps_data; /* kill warning */
    (void)token;

    char *filename;
    if (token->type == SKIN_TOKEN_IMAGE_BACKDROP)
    {
        if (isdefault(get_param(element, 0)))
        {
            filename = "-";
        }
        else
        {
            filename = get_param_text(element, 0);
            /* format: %X(filename.bmp) or %X(d) */
            if (!strcmp(filename, "d"))
                filename = NULL;
        }
        backdrop_filename = filename;
    }

    return 0;
}

static int parse_progressbar_tag(struct skin_element* element,
                                 struct wps_token *token,
                                 struct wps_data *wps_data);

static int parse_setting_and_lang(struct skin_element *element,
                                  struct wps_token *token,
                                  struct wps_data *wps_data)
{
    (void)wps_data;
    char *temp = get_param_text(element, 0);

    if (token->type == SKIN_TOKEN_TRANSLATEDSTRING)
    {
        int i = lang_english_to_id(temp);
        if (i < 0) {
            DEBUGF("Translated String [%s] NOT FOUND\n", temp);
            /* Due to conditionals in a theme, a missing string
               might never be hit.  So currently we have to just treat
               this as an advisory */
#if 1
            i = LANG_LAST_INDEX_IN_ARRAY;
#else
            return WPS_ERROR_INVALID_PARAM;
#endif
        }
#ifdef DEBUG_SKIN_ENGINE
        else if (debug_wps) {
            DEBUGF("Translated String [%s] = %d\n", temp, i);
        }
#endif
        token->value.i = i;
    }
    else if (element->params_count > 1)
    {
        if (element->params_count > 4)
            return parse_progressbar_tag(element, token, wps_data);
        else
            return WPS_ERROR_INVALID_PARAM;
    }
    else
    {
        const struct settings_list *setting = find_setting_by_cfgname(temp);
        if (!setting) {
            DEBUGF("Invalid setting [%s]\n", temp);
            return WPS_ERROR_INVALID_PARAM;
        }
        token->value.xdata = (void *)setting;
    }
    return 0;
}

static int parse_logical_andor(struct skin_element *element,
                             struct wps_token *token,
                             struct wps_data *wps_data)
{
    (void)wps_data;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, element);
    return 0;
}

/* %tw(tag) -- keep just the token being measured, the way %ss keeps its own
 * text argument, so the render side has no argument list to walk. */
/* Tags whose whole argument list is walked at render time and whose arg count
 * is already enforced by their param spec: just stash the element. */
static int parse_store_element(struct skin_element *element,
                               struct wps_token *token,
                               struct wps_data *wps_data)
{
    (void)wps_data;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, element);
    return 0;
}

/* %wr(n, text) -- keep the whole element; the render side reads the line index
 * (param 0) and wraps the text (param 1) to the current viewport width. */
static int parse_wordwrap(struct skin_element *element,
                          struct wps_token *token,
                          struct wps_data *wps_data)
{
    (void)wps_data;
    if (element->params_count != 2)
        return WPS_ERROR_INVALID_PARAM;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, element);
    return 0;
}

/* %wt(text[,align]) -- store the text token plus a 2-char alignment code
 * (vertical t/c/b then horizontal l/c/r); the render side wraps and draws it. */
static int parse_textbox(struct skin_element *element,
                         struct wps_token *token,
                         struct wps_data *wps_data)
{
    (void)wps_data;
    struct skin_textbox *tb = skin_buffer_alloc(sizeof(*tb));
    if (!tb)
        return 1;
    tb->token = get_param_code(element, 0)->data;
    tb->fallback = INVALID_OFFSET;
    tb->valign = 't';
    tb->halign = 'l';
    if (element->params_count > 1)
    {
        const char *a = get_param_text(element, 1);
        if (a && a[0])
        {
            tb->valign = tolower(a[0]);
            if (a[1])
                tb->halign = tolower(a[1]);
        }
    }
    if (element->params_count > 2)
        tb->fallback = get_param_code(element, 2)->data;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, tb);
    return 0;
}

/* %sel(subject, key1, value1, ..., [default]) -- the whole argument list is
 * kept and walked at render time, so this only has to check the shape. After
 * the subject the arguments pair up; a lone trailing one is the default. */
static int parse_select(struct skin_element *element,
                        struct wps_token *token,
                        struct wps_data *wps_data)
{
    (void)wps_data;
    if (element->params_count < 3)
        return WPS_ERROR_INVALID_PARAM;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, element);
    return 0;
}

static int parse_logical_if(struct skin_element *element,
                             struct wps_token *token,
                             struct wps_data *wps_data)
{
    (void)wps_data;
    char *op = get_param_text(element, 1);
    struct logical_if *lif = skin_buffer_alloc(sizeof(*lif));
    if (!lif)
        return -1;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, lif);
    lif->token = get_param_code(element, 0)->data;

    /* one or two operator conditionals */
    #define OPS2VAL(op1, op2) ((int)op1 << 8 | (int)op2)
    #define CLAUSE(op1, op2, symbol) {OPS2VAL(op1, op2), symbol }

    struct clause_symbol {int value;int symbol;};
    static const struct clause_symbol get_clause_match[] =
    {
        CLAUSE('=', '=', IF_EQUALS),
        CLAUSE('!', '=', IF_NOTEQUALS),
        CLAUSE('>', '=', IF_GREATERTHAN_EQ),
        CLAUSE('<', '=', IF_LESSTHAN_EQ),
        /*All Single value items @ end */
        CLAUSE('>', 0, IF_GREATERTHAN),
        CLAUSE('<', 0, IF_LESSTHAN),
        CLAUSE('=', 0, IF_EQUALS),
    };

    int val1 = OPS2VAL(op[0], 0);
    int val2;
    if (val1 != 0) /* Empty string ?*/
    {
        val2 = OPS2VAL(op[0], op[1]);
        for (unsigned int i = 0; i < ARRAYLEN(get_clause_match); i++)
        {
            const struct clause_symbol *sym = &get_clause_match[i];
            if(sym->value == val1 || sym->value == val2)
            {
                lif->op = sym->symbol;
                break;
            }
        }
    }
    memcpy(&lif->operand, get_param(element, 2), sizeof(lif->operand));
    if (element->params_count > 3)
        lif->num_options = get_param(element, 3)->data.number;
    else
        lif->num_options = TOKEN_VALUE_ONLY;
    return 0;

}

static int parse_timeout_tag(struct skin_element *element,
                             struct wps_token *token,
                             struct wps_data *wps_data)
{
    (void)wps_data;
    int val = 0;
    int params_count = element->params_count;
    if (params_count == 0)
    {
        switch (token->type)
        {
            case SKIN_TOKEN_SUBLINE_TIMEOUT:
                return -1;
            case SKIN_TOKEN_BUTTON_VOLUME:
            case SKIN_TOKEN_TRACK_STARTING:
            case SKIN_TOKEN_TRACK_ENDING:
                val = 10;
                break;
            default:
                break;
        }
    }
    else
    {
        val = get_param(element, 0)->data.number;
        if (token->type == SKIN_TOKEN_SUBLINE_TIMEOUT && params_count == 2)
        {
            struct wps_subline_timeout *st = skin_buffer_alloc(sizeof(*st));
            if (st)
            {
                st->show = val;
                st->hide = get_param(element, 1)->data.number;
                st->next_tick = current_tick; /* show immediately the first time */
                token->type = SKIN_TOKEN_SUBLINE_TIMEOUT_HIDE;
                token->value.data = PTRTOSKINOFFSET(skin_buffer, st);
                return 0;
            }
        }
    }
    token->value.i = val * TIMEOUT_UNIT;
    return 0;
}

static int parse_substring_tag(struct skin_element* element,
                                 struct wps_token *token,
                                 struct wps_data *wps_data)
{
    (void)wps_data;
    struct substring *ss = skin_buffer_alloc(sizeof(*ss));
    if (!ss)
        return 1;
    ss->start = get_param(element, 0)->data.number;
    if (get_param(element, 1)->type == DEFAULT)
        ss->length = -1;
    else
        ss->length = get_param(element, 1)->data.number;
    ss->token = get_param_code(element, 2)->data;
    if (element->params_count > 3)
        ss->expect_number = !strcmp(get_param_text(element, 3), "number");
    else
        ss->expect_number = false;
    token->value.data = PTRTOSKINOFFSET(skin_buffer, ss);
    return 0;
}

static int parse_progressbar_tag(struct skin_element* element,
                                 struct wps_token *token,
                                 struct wps_data *wps_data)
{
    struct progressbar *pb;
    struct viewport *vp = &curr_vp->vp;
    struct skin_tag_parameter *param = get_param(element, 0);
    int curr_param = 0;
    int setting_offset = 0;
    char *image_filename = NULL;

    if (element->params_count == 0 &&
        element->tag->type != SKIN_TOKEN_PROGRESSBAR)
        return 0; /* nothing to do */
    pb = skin_buffer_alloc(sizeof(*pb));

    token->value.data = PTRTOSKINOFFSET(skin_buffer, pb);

    if (!pb)
        return WPS_ERROR_INVALID_PARAM;
    pb->follow_lang_direction = follow_lang_direction > 0;
    pb->nofill = false;
    pb->noborder = false;
    pb->nobar = false;
    pb->image = PTRTOSKINOFFSET(skin_buffer, NULL);
    pb->slider = PTRTOSKINOFFSET(skin_buffer, NULL);
    pb->backdrop = PTRTOSKINOFFSET(skin_buffer, NULL);
    pb->setting = NULL;
    pb->invert_fill_direction = false;
    pb->horizontal = true;

    if (element->params_count == 0)
    {
        pb->x = 0;
        pb->width = vp->width;
        pb->height = SYSFONT_HEIGHT-2;
        pb->y = -1; /* Will be computed during the rendering */
        pb->type = element->tag->type;
        return 0;
    }

    /* (x, y, width, height, ...) */
    if (!isdefault(param))
    {
        pb->x = percent_parse_param(param, vp->width);

        if (pb->x < 0 || pb->x >= vp->width)
            return WPS_ERROR_INVALID_PARAM;
    }
    else
        pb->x = 0;
    param++;

    if (!isdefault(param))
    {
        pb->y = percent_parse_param(param, vp->height);

        if (pb->y < 0 || pb->y >= vp->height)
            return WPS_ERROR_INVALID_PARAM;
    }
    else
        pb->y = -1; /* computed at rendering */
    param++;

    if (!isdefault(param))
    {
        pb->width = percent_parse_param(param, vp->width);

        if (pb->width <= 0 || (pb->x + pb->width) > vp->width)
            return WPS_ERROR_INVALID_PARAM;
    }
    else
        pb->width = vp->width - pb->x;
    param++;

    if (!isdefault(param))
    {
        int max;
        pb->height = percent_parse_param(param, vp->height);

        /* include y in check only if it was non-default */
        max = (pb->y > 0) ? pb->y + pb->height : pb->height;
        if (pb->height <= 0 || max > vp->height)
            return WPS_ERROR_INVALID_PARAM;
    }
    else
    {
        if (vp->font > FONT_UI)
            pb->height = -1; /* calculate at display time */
        else
        {
            pb->height = font_get(vp->font)->height;
        }
    }
    /* optional params, first is the image filename if it isnt recognised as a keyword */

    curr_param = 4;
    if (isdefault(get_param(element, curr_param)))
    {
        param++;
        curr_param++;
    }

    pb->horizontal = pb->width > pb->height;

    enum
    {
        eINVERT = 0, eNOFILL, eNOBORDER, eNOBAR, eSLIDER, eIMAGE,
        eBACKDROP, eVERTICAL, eHORIZONTAL, eNOTOUCH, eSETTING, eSETTING_OFFSET,
        e_PB_TAG_COUNT
    };

    static const char *pb_options[e_PB_TAG_COUNT + 1] = {[eINVERT] = "invert",
                 [eNOFILL] = "nofill", [eNOBORDER] = "noborder", [eNOBAR] = "nobar",
                 [eSLIDER] = "slider", [eIMAGE] = "image", [eBACKDROP] = "backdrop",
                 [eVERTICAL] = "vertical", [eHORIZONTAL] = "horizontal",
                 [eNOTOUCH] = "notouch", [eSETTING] = "setting", [eSETTING_OFFSET] = "soffset", [e_PB_TAG_COUNT] = NULL};
    int pb_op;

    while (curr_param < element->params_count)
    {
        char* text;
        param++;
        text = SKINOFFSETTOPTR(skin_buffer, param->data.text);

        pb_op = string_option(text, pb_options, false);
        if (pb_op == eINVERT)
            pb->invert_fill_direction = true;
        else if (pb_op == eNOFILL)
            pb->nofill = true;
        else if (pb_op == eNOBORDER)
            pb->noborder = true;
        else if (pb_op == eNOBAR)
            pb->nobar = true;
        else if (pb_op == eSLIDER)
        {
            if (curr_param+1 < element->params_count)
            {
                curr_param++;
                param++;
                text = SKINOFFSETTOPTR(skin_buffer, param->data.text);
                pb->slider = PTRTOSKINOFFSET(skin_buffer,
                        skin_find_item(text, SKIN_FIND_IMAGE, wps_data));
            }
            else /* option needs the next param */
                return -1;
        }
        else if (pb_op == eIMAGE)
        {
            if (curr_param+1 < element->params_count)
            {
                curr_param++;
                param++;
                image_filename = SKINOFFSETTOPTR(skin_buffer, param->data.text);
            }
            else /* option needs the next param */
                return -1;
        }
        else if (pb_op == eBACKDROP)
        {
            if (curr_param+1 < element->params_count)
            {
                curr_param++;
                param++;
                text = SKINOFFSETTOPTR(skin_buffer, param->data.text);
                pb->backdrop = PTRTOSKINOFFSET(skin_buffer,
                        skin_find_item(text, SKIN_FIND_IMAGE, wps_data));

            }
            else /* option needs the next param */
                return -1;
        }
        else if (pb_op == eVERTICAL)
        {
            pb->horizontal = false;
            if (isdefault(get_param(element, 3)))
                pb->height = vp->height - pb->y;
        }
        else if (pb_op == eHORIZONTAL)
            pb->horizontal = true;
        else if (token->type == SKIN_TOKEN_SETTING && pb_op == eSETTING_OFFSET)
        {
            if (curr_param+1 < element->params_count)
            {
                curr_param++;
                param++;
                setting_offset = param->data.number;
            }
        }
        else if (token->type == SKIN_TOKEN_SETTING && pb_op == eSETTING)
        {
            if (curr_param+1 < element->params_count)
            {
                curr_param++;
                param++;
                text = SKINOFFSETTOPTR(skin_buffer, param->data.text);
                pb->setting = find_setting_by_cfgname(text);
                if (!pb->setting)
                    return WPS_ERROR_INVALID_PARAM;
                pb->setting_offset = setting_offset;
            }
        }
        else if (curr_param == 4)
            image_filename = text;

        curr_param++;
    }

    if (image_filename)
    {
        /* noborder is incompatible together with image. There is no border
         * anyway. */
        if (pb->noborder)
            return WPS_ERROR_INVALID_PARAM;

        pb->image = PTRTOSKINOFFSET(skin_buffer,
                skin_find_item(image_filename, SKIN_FIND_IMAGE, wps_data));
        if (!SKINOFFSETTOPTR(skin_buffer, pb->image)) /* load later */
        {
            struct gui_img *img = skin_buffer_alloc(sizeof(*img));
            if (!img)
                return WPS_ERROR_INVALID_PARAM;
            /* save a pointer to the filename */
            img->bm.data = (char*)image_filename;
            img->label = PTRTOSKINOFFSET(skin_buffer, image_filename);
            img->x = 0;
            img->y = 0;
            img->num_subimages = 1;
            img->display = -1;
            img->using_preloaded_icons = false;
            img->buflib_handle = -1;
            img->loaded = false;
            struct skin_token_list *item = new_skin_token_list_item(NULL, img);
            if (!item)
                return WPS_ERROR_INVALID_PARAM;
            add_to_ll_chain(&wps_data->images, item);
            pb->image = PTRTOSKINOFFSET(skin_buffer, img);
        }
    }

    if (token->type == SKIN_TOKEN_VOLUME)
        token->type = SKIN_TOKEN_VOLUMEBAR;
    else if (token->type == SKIN_TOKEN_BATTERY_PERCENT)
        token->type = SKIN_TOKEN_BATTERY_PERCENTBAR;
    else if (token->type == SKIN_TOKEN_TUNER_RSSI)
        token->type = SKIN_TOKEN_TUNER_RSSI_BAR;
    else if (token->type == SKIN_TOKEN_PEAKMETER_LEFT)
        token->type = SKIN_TOKEN_PEAKMETER_LEFTBAR;
    else if (token->type == SKIN_TOKEN_PEAKMETER_RIGHT)
        token->type = SKIN_TOKEN_PEAKMETER_RIGHTBAR;
    else if (token->type == SKIN_TOKEN_PLAYLIST_PERCENT)
        token->type = SKIN_TOKEN_PLAYLIST_PERCENTBAR;
    else if (token->type == SKIN_TOKEN_PLAYLIST_ELAPSED_PERCENT)
    {
        wps_playlist_percent_enable(); /* enable scanning track lengths */
        token->type = SKIN_TOKEN_PLAYLIST_PROGRESSBAR;
    }
    else if (token->type == SKIN_TOKEN_LIST_NEEDS_SCROLLBAR)
        token->type = SKIN_TOKEN_LIST_SCROLLBAR;
    else if (token->type == SKIN_TOKEN_SETTING)
        token->type = SKIN_TOKEN_SETTINGBAR;

    pb->type = token->type;

    return 0;
}

static int parse_filetext(struct skin_element *element,
                            struct wps_token *token,
                            struct wps_data *wps_data)
{
    (void)wps_data;
    const char* filename;
    char buf[MAX_PATH];
    char *buf_start = buf;
    int fd;
    int line = 0;
    const char *search_text = NULL;
    int search_len = 0;

    /* format: %ft(filename[,line|search text]) */
    filename = get_param_text(element, 0);

    if (element->params_count == 2)
    {
        struct skin_tag_parameter *param1 = get_param(element, 1);
        if (param1->type == INTEGER)
            line = param1->data.number;
        else if (param1->type == STRING)
        {
            search_text = get_param_text(element, 1);
            line = 0x3FF; /* 1k lines is a pretty large file */
            /* Trim whitespace around the key so "key:" and "key: " match the
             * same line -- the value's own whitespace is skipped after the
             * match below. Without this the key had to be written to include
             * the exact spacing used in the file. */
            while (*search_text == ' ' || *search_text == '\t')
                search_text++;
            search_len = strlen(search_text);
            while (search_len > 0 && (search_text[search_len - 1] == ' '
                                   || search_text[search_len - 1] == '\t'))
                search_len--;
            DEBUGF("%s: found search text %s\n", __func__, search_text);
        }
        else
            return WPS_ERROR_INVALID_PARAM;
    }
    else if (element->params_count != 1)
    {
        DEBUGF("%s(file, line): %s Error: param count %d\n",
                  __func__, filename, element->params_count);
        return WPS_ERROR_INVALID_PARAM;
    }

    while(*filename == PATH_SEPCH) /* no absolute paths! */
        filename++;

    path_append(buf, ROCKBOX_DIR, filename, sizeof(buf));
    DEBUGF("%s %s[%d]\n", __func__, buf, line);

    if (line > 0x3FF || (fd = open_utf8(buf, O_RDONLY)) < 0)
    {
        DEBUGF("%s: Error Opening %s\n", __func__, buf);
        goto failure;
    }

    int rd = 0;
    while (line >= 0)
    {
        if ((rd = read_line(fd, buf, sizeof(buf))) <= 0)
            break;
        if (search_text && strncasecmp(buf, search_text, search_len) == 0)
        {
            buf_start += search_len;
            rd -= search_len;
            /* skip whitespace between the key and its value */
            while (*buf_start == ' ' || *buf_start == '\t')
            {
                buf_start++;
                rd--;
            }
            DEBUGF("%s: found '%s' Reading '%s'\n", __func__, search_text, buf);
            break;
        }
        line--;
    }

    if (rd <= 0) /* empty line? */
    {
        DEBUGF("%s: Error(%d) Reading %s\n", __func__, rd, filename);
        close(fd);
        goto failure;
    }

    if (element->is_conditional)
    {
        buf_start = buf;
        rd = 1; /* just alloc enough for the conditional to work*/
    }

    buf_start[rd] = '\0';

    char * skinbuf = skin_buffer_alloc(rd+1);

    if (!skinbuf)
    {
        DEBUGF("%s: Error No Buffer %s\n", __func__, filename);
        close(fd);
        return WPS_ERROR_INVALID_PARAM;
    }
    strcpy(skinbuf, buf_start);
    close(fd);
    token->value.data = PTRTOSKINOFFSET(skin_buffer, skinbuf);
    token->type = SKIN_TOKEN_STRING;
    return 0;
failure:
    element->type = COMMENT;
    element->data = INVALID_OFFSET;
    token->type = SKIN_TOKEN_NO_TOKEN;
    token->value.data = INVALID_OFFSET;
    return 0;
}


static int parse_albumart_load(struct skin_element* element,
                               struct wps_token *token,
                               struct wps_data *wps_data)
{
    struct dim dimensions;
    struct skin_token_list *item;
    bool swap_for_rtl = lang_is_rtl() && follow_lang_direction;
    struct skin_albumart *aa = skin_buffer_alloc(sizeof(*aa));
    (void)token; /* silence warning */
    if (!aa)
        return -1;

    /* reset albumart info in wps */
    aa->xalign = WPS_ALBUMART_ALIGN_CENTER; /* default */
    aa->yalign = WPS_ALBUMART_ALIGN_CENTER; /* default */

    aa->x = percent_parse_param(get_param(element, 0), curr_vp->vp.width);
    aa->y =  percent_parse_param(get_param(element, 1), curr_vp->vp.height);
    aa->width =  percent_parse_param(get_param(element, 2), curr_vp->vp.width);
    aa->height =  percent_parse_param(get_param(element, 3), curr_vp->vp.height);
    aa->draw_handle = -1;

    /* if we got here, we parsed everything ok .. ! */
    if (aa->width < 0)
        aa->width = 0;

    if (aa->height < 0)
        aa->height = 0;

    if (swap_for_rtl)
        aa->x = (curr_vp->vp.width - aa->width - aa->x);

    aa->filter_handle = -1;
    aa->filtered_art = -1;
    aa->filtered_gen = 0;       /* skin_albumart_gen() never returns 0 */
    aa->filter_avoid = -1;
    aa->slot = -1;
    aa->label = PTRTOSKINOFFSET(skin_buffer, NULL);
    aa->draw_win = PTRTOSKINOFFSET(skin_buffer, NULL);

    /* The filter chain has to be compiled before the slot is claimed: a
     * blurred chain wants a much smaller source than it draws, and the size
     * asked for here is what every buffered track will carry. Compiled here
     * rather than at draw time, so a misspelt filter fails the skin at load
     * instead of quietly drawing nothing. A skin that named no chain
     * compiles the empty one, which has no stages. */
    const char *chain = NULL;
    if (element->params_count > 6 && !isdefault(get_param(element, 6)))
        chain = get_param_text(element, 6);

    if (!img_filter_compile(chain, IMG_CLASSES_ALL, &aa->filter))
    {
        DEBUGF("%%Cl: %s\n", aa->filter.error);
        return WPS_ERROR_INVALID_PARAM;
    }

    /* Eighth parameter: corner radius. Clamped to the box rather than to the
     * art, which is only known once a track has one -- art smaller than the
     * box than falls back to square corners at draw time rather than taking
     * a curve too big for it. */
    aa->radius = 0;
    aa->mask = PTRTOSKINOFFSET(skin_buffer, NULL);
    if (element->params_count > 7 && !isdefault(get_param(element, 7)))
    {
        int radius = get_param(element, 7)->data.number;
        unsigned char *mask;

        if (radius < 0 || radius > ROUND_RECT_MAX_RADIUS)
            return WPS_ERROR_INVALID_PARAM;
        if (radius > aa->width / 2)
            radius = aa->width / 2;
        if (radius > aa->height / 2)
            radius = aa->height / 2;

        if (radius > 0)
        {
            mask = skin_buffer_alloc(round_rect_mask_size(radius));
            if (!mask)
                return -1;
            round_rect_build_mask(mask, radius, LCD_BLEND_OPAQUE);
            aa->radius = (uint8_t)radius;
            aa->mask = PTRTOSKINOFFSET(skin_buffer, mask);
        }
    }

    /* Ninth parameter: the name a %Cd refers to this art by. Names have to be
     * unique within the skin or a %Cd could not say which art it meant. */
    if (element->params_count > 8 && !isdefault(get_param(element, 8)))
    {
        const char *label = get_param_text(element, 8);

        if (skin_find_item(label, SKIN_FIND_ALBUMART, wps_data))
        {
            DEBUGF("%%Cl: duplicate label %s\n", label);
            return WPS_ERROR_INVALID_PARAM;
        }
        aa->label = PTRTOSKINOFFSET(skin_buffer, (void*)label);
    }

    /* Slots dedupe by dimension, so a blurred %Cl no longer shares one with
     * a same-sized unblurred one. That is right -- they want different
     * pixels -- and it is what keeps the saving: audio_load_albumart() runs
     * per buffered track, so a full-screen slot costs ~115 KiB per track
     * where the decimated one costs about seven.
     *
     * The same dedupe is what makes several %Cl affordable: any that agree on
     * size share the one buffered bitmap, whether they are in this skin or the
     * other one. Only a new size costs anything, and there are only
     * MAX_MULTIPLE_AA of those to go round -- past that the claim fails and
     * the art simply does not draw. */
    int div = img_filter_source_divisor(&aa->filter, aa->width, aa->height);
    dimensions.width = (aa->width + div - 1) / div;
    dimensions.height = (aa->height + div - 1) / div;

    aa->slot = playback_claim_aa_slot(&dimensions);

    /* Appended only now that nothing can still fail: an entry in the chain is
     * one skin_data_free_buflib_allocs() will release a slot for. */
    item = new_skin_token_list_item(NULL, aa);
    if (!item)
    {
        playback_release_aa_slot(aa->slot);
        return WPS_ERROR_INVALID_PARAM;
    }
    add_to_ll_chain(&wps_data->albumart, item);
    curr_albumart = aa;   /* what a following bare %Cd will draw */

    if (element->params_count > 4 && !isdefault(get_param(element, 4)))
    {
        switch (tolower(*get_param_text(element, 4)))
        {
            case 'l':
                if (swap_for_rtl)
                    aa->xalign = WPS_ALBUMART_ALIGN_RIGHT;
                else
                    aa->xalign = WPS_ALBUMART_ALIGN_LEFT;
                break;
            case 'c':
                aa->xalign = WPS_ALBUMART_ALIGN_CENTER;
                break;
            case 'r':
                if (swap_for_rtl)
                    aa->xalign = WPS_ALBUMART_ALIGN_LEFT;
                else
                    aa->xalign = WPS_ALBUMART_ALIGN_RIGHT;
                break;
        }
    }
    if (element->params_count > 5 && !isdefault(get_param(element, 5)))
    {
        switch (tolower(*get_param_text(element, 5)))
        {
            case 't':
                aa->yalign = WPS_ALBUMART_ALIGN_TOP;
                break;
            case 'c':
                aa->yalign = WPS_ALBUMART_ALIGN_CENTER;
                break;
            case 'b':
                aa->yalign = WPS_ALBUMART_ALIGN_BOTTOM;
                break;
        }
    }

    return 0;
}

/* %Cd([label][,x,y,w,h]): draw one of the skin's %Cl declarations, optionally
 * through a window -- see struct skin_albumart_draw.
 *
 * A bare %Cd draws the nearest %Cl above it, which is what the load-then-draw
 * pairing reads as and what skins written before labels existed rely on --
 * `%Cl(a)%Cd%Cl(b)%Cd` draws a then b. With one %Cl, every %Cd means it, so
 * nothing about an ordinary skin changes.
 *
 * A %Cd above every %Cl leaves the art NULL and is resolved at render time to
 * the skin's first. A named one must already exist, the same rule %xd follows
 * for images. */
static int parse_albumart_display(struct skin_element* element,
                                  struct wps_token *token,
                                  struct wps_data *wps_data)
{
    struct skin_albumart_draw *ad = skin_buffer_alloc(sizeof(*ad));
    struct skin_albumart *aa = curr_albumart;
    int given = 0;

    if (!ad)
        return -1;

    ad->x = ad->y = ad->w = ad->h = 0;   /* w == 0: the whole art box */

    if (element->params_count > 0 && !isdefault(get_param(element, 0)))
    {
        const char *label = get_param_text(element, 0);

        aa = skin_find_item(label, SKIN_FIND_ALBUMART, wps_data);
        if (!aa)
        {
            DEBUGF("%%Cd: no %%Cl called %s\n", label);
            return WPS_ERROR_INVALID_PARAM;
        }
    }

    /* The window is all four or none: three of them describes no rectangle,
     * and silently ignoring the ones that were written would hide the typo. */
    for (int i = 1; i <= 4; i++)
    {
        if (element->params_count > i && !isdefault(get_param(element, i)))
            given++;
    }

    if (given == 4)
    {
        ad->x = percent_parse_param(get_param(element, 1), curr_vp->vp.width);
        ad->y = percent_parse_param(get_param(element, 2), curr_vp->vp.height);
        ad->w = percent_parse_param(get_param(element, 3), curr_vp->vp.width);
        ad->h = percent_parse_param(get_param(element, 4), curr_vp->vp.height);

        if (ad->w <= 0 || ad->h <= 0)
        {
            DEBUGF("%%Cd: window has no area\n");
            return WPS_ERROR_INVALID_PARAM;
        }
    }
    else if (given != 0)
    {
        DEBUGF("%%Cd: a window needs all of x, y, width and height\n");
        return WPS_ERROR_INVALID_PARAM;
    }

    ad->art = PTRTOSKINOFFSET(skin_buffer, aa);
    token->value.data = PTRTOSKINOFFSET(skin_buffer, ad);
    return 0;
}

/* Skin variables. A skin refers to one by name; the first tag that mentions a
 * name creates it, so %vg can read a variable a later %vs writes. */
static struct skin_var* find_or_add_var(const char* label,
                                        struct wps_data *data)
{
    struct skin_var* ret = skin_find_item(label, SKIN_VARIABLE, data);
    if (ret)
        return ret;
    ret = skin_buffer_alloc(sizeof(*ret));
    if (!ret)
        return ret;
    ret->label = PTRTOSKINOFFSET(skin_buffer, label);
    ret->value = 1;
    ret->last_changed = 0xffff; /* "never", as %vl tests for */
    struct skin_token_list *item = new_skin_token_list_item(NULL, ret);
    if (!item)
        return NULL;
    add_to_ll_chain(&data->skinvars, item);
    return ret;
}

static int parse_skinvar(  struct skin_element *element,
                           struct wps_token *token,
                           struct wps_data *wps_data)
{
    const char* label = get_param_text(element, 0);
    struct skin_var* var = find_or_add_var(label, wps_data);
    if (!var)
        return WPS_ERROR_INVALID_PARAM;
    switch (token->type)
    {
        case SKIN_TOKEN_VAR_GETVAL:
            token->value.data = PTRTOSKINOFFSET(skin_buffer, var);
            return 0;
        case SKIN_TOKEN_VAR_SET:
        {
            static const char * const sv_options[] = {"touch", "set", "inc", "dec", NULL};
            struct skin_var_changer *data = skin_buffer_alloc(sizeof(*data));
            if (!data)
                return WPS_ERROR_INVALID_PARAM;
            data->var = PTRTOSKINOFFSET(skin_buffer, var);
            char *text_param1 = get_param_text(element, 1);
            int sv_op = string_option(text_param1, sv_options, false);
            if (!isdefault(get_param(element, 2)))
                data->newval = get_param(element, 2)->data.number;
            else if (sv_op != 0) /*!touch*/
                return WPS_ERROR_INVALID_PARAM;
            data->max = 0;
            if (sv_op == 1) /*set*/
                data->direct = true;
            else if (sv_op == 2) /*inc*/
            {
                data->direct = false;
            }
            else if (sv_op == 3) /*dec*/
            {
                data->direct = false;
                data->newval *= -1;
            }
            else if (sv_op == 0) /*touch*/
            {
                data->direct = false;
                data->newval = 0;
            }
            if (element->params_count > 3)
                data->max = get_param(element, 3)->data.number;
            token->value.data = PTRTOSKINOFFSET(skin_buffer, data);
        }
        return 0;
        case SKIN_TOKEN_VAR_TIMEOUT:
        {
            struct skin_var_lastchange *data = skin_buffer_alloc(sizeof(*data));
            if (!data)
                return WPS_ERROR_INVALID_PARAM;
            data->var = PTRTOSKINOFFSET(skin_buffer, var);
            data->timeout = 10;
            if (element->params_count > 1)
                data->timeout = get_param(element, 1)->data.number;
            data->timeout *= TIMEOUT_UNIT;
            token->value.data = PTRTOSKINOFFSET(skin_buffer, data);
        }
        default:
            return 0;
    }
}

static bool check_feature_tag(const int type)
{
    switch (type)
    {
        case SKIN_TOKEN_RTC_PRESENT:
            return true;
        case SKIN_TOKEN_HAVE_RECORDING:
            return false;
        case SKIN_TOKEN_HAVE_TUNER:
            return false;
        case SKIN_TOKEN_HAVE_TOUCH:
            return false;

        default: /* not a tag we care about, just don't skip */
            return true;
    }
}

/* This is used to free any buflib allocations before the rest of
 * wps_data is reset.
 * The call to this in settings_apply_skins() is the last chance to do
 * any core_free()'s before wps_data is trashed and those handles lost
 */
void skin_data_free_buflib_allocs(struct wps_data *wps_data)
{
    if (wps_data->wps_loaded)
        skin_buffer = get_skin_buffer(wps_data);

    if (!skin_buffer)
        goto abort;

    struct skin_token_list *list = SKINOFFSETTOPTR(skin_buffer, wps_data->images);
    int16_t *font_ids = SKINOFFSETTOPTR(skin_buffer, wps_data->font_ids);
    while (list)
    {
        struct wps_token *token = SKINOFFSETTOPTR(skin_buffer, list->token);
        struct gui_img *img = NULL;
        if (token)
            img = (struct gui_img*)SKINOFFSETTOPTR(skin_buffer, token->value.data);
        if (img && img->buflib_handle > 0)
        {
            struct skin_token_list *imglist = SKINOFFSETTOPTR(skin_buffer, list->next);

            core_free(img->buflib_handle);
            while (imglist)
            {
                struct wps_token *freetoken = SKINOFFSETTOPTR(skin_buffer, imglist->token);
                struct gui_img *freeimg = NULL;
                if (freetoken)
                    freeimg = (struct gui_img*)SKINOFFSETTOPTR(skin_buffer, freetoken->value.data);
                if (freeimg && img->buflib_handle == freeimg->buflib_handle)
                    freeimg->buflib_handle = -1;
                imglist = SKINOFFSETTOPTR(skin_buffer, imglist->next);
            }
        }
        list = SKINOFFSETTOPTR(skin_buffer, list->next);
    }
    if (font_ids != NULL)
    {
        while (wps_data->font_count > 0)
            font_unload(font_ids[--wps_data->font_count]);
    }

    /* Both jobs are done here rather than in skin_data_reset(): the album art
     * chain lives in the buffer this function goes on to free, so by the time
     * that runs the filter handles and the slots they were claimed against
     * can no longer be read. Clearing the chain below makes a second call a
     * no-op, so no slot is released twice. */
    {
        struct skin_token_list *node =
                SKINOFFSETTOPTR(skin_buffer, wps_data->albumart);

        while (node)
        {
            struct skin_albumart *aa = skin_albumart_of(skin_buffer, node);

            if (aa)
            {
                if (aa->filter_handle > 0)
                    aa->filter_handle = core_free(aa->filter_handle);
                playback_release_aa_slot(aa->slot);
            }
            node = SKINOFFSETTOPTR(skin_buffer, node->next);
        }
    }

abort:
    wps_data->font_ids = PTRTOSKINOFFSET(skin_buffer, NULL); /* Safe if skin_buffer is NULL */
    wps_data->images = PTRTOSKINOFFSET(skin_buffer, NULL);
    wps_data->albumart = PTRTOSKINOFFSET(skin_buffer, NULL);
    if (wps_data->buflib_handle > 0)
        core_unpin(wps_data->buflib_handle); /* balance the pin in skin_data_load() */
    wps_data->buflib_handle = core_free(wps_data->buflib_handle);
}

/*
 * initial setup of wps_data; does reset everything
 * except fields which need to survive, i.e.
 * Also called if the load fails
 **/
static void skin_data_reset(struct wps_data *wps_data)
{
    skin_data_free_buflib_allocs(wps_data);
    wps_data->images = INVALID_OFFSET;
    wps_data->tree = INVALID_OFFSET;
    wps_data->skinvars = INVALID_OFFSET;
    if (wps_data->backdrop_id >= 0)
        skin_backdrop_unload(wps_data->backdrop_id);
    backdrop_filename = NULL;
    wps_data->albumart = INVALID_OFFSET;   /* slots already released above */

    wps_data->peak_meter_enabled = false;
    wps_data->draws_quickscreen = false;
    wps_data->wps_sb_tag = false;
    wps_data->show_sb_on_wps = false;
    wps_data->wps_loaded = false;
}

static int buflib_move_callback(int handle, void* current, void* new)
{
    (void)handle;
    (void)current;
    (void)new;
    /* Any active skins may be scrolling - which means using viewports which
     * will be moved after this callback returns. This is a hammer to make that
     * safe. TODO: use a screwdriver instead.
     */
    FOR_NB_SCREENS(i)
        screens[i].scroll_stop();

    for (int i = 0; i < SKINNABLE_SCREENS_COUNT; i++)
        skin_request_full_update(i);

    return BUFLIB_CB_OK;
}

static int load_skin_bmp(struct wps_data *wps_data, struct gui_img *img, char* bmpdir)
{

    (void)wps_data; /* only needed for remote targets */
    char img_path[MAX_PATH];
    struct bitmap *bitmap = &img->bm;

    get_image_filename(bitmap->data, bmpdir,
                       img_path, sizeof(img_path));

    static struct buflib_callbacks buflib_ops = {buflib_move_callback, NULL, NULL};
    int handle;
    int bmpformat;
    ssize_t buf_reqd;
    bmpformat = img->dither ? FORMAT_ANY|FORMAT_DITHER|FORMAT_TRANSPARENT :
                              FORMAT_ANY|FORMAT_TRANSPARENT;

    handle = core_load_bmp(img_path, bitmap, bmpformat, &buf_reqd, &buflib_ops);
    if (handle != CLB_ALOC_ERR)
    {
        /* NOTE!: bitmap->data == NULL to force a crash later if the
           caller doesnt call core_get_data() */
        _stats->buflib_handles++;
        _stats->images_size += buf_reqd;
        return handle;
    }

    if (buf_reqd == CLB_READ_ERR)
    {
        /* Abort if we can't load an image */
        DEBUGF("Couldn't load '%s' (%zu)\n", img_path, buf_reqd);
    }
    else
    {
        DEBUGF("Not enough skin buffer: need %zd more.\n",
                buf_reqd - skin_buffer_freespace());
    }

    return -1;
}

static bool load_skin_bitmaps(struct wps_data *wps_data, char *bmpdir)
{
    struct skin_token_list *list;
    bool retval = true; /* return false if a single image failed to load */

    /* regular images */
    list = SKINOFFSETTOPTR(skin_buffer, wps_data->images);
    while (list)
    {
        struct wps_token *token = SKINOFFSETTOPTR(skin_buffer, list->token);
        if (!token) goto skip;
        struct gui_img *img = (struct gui_img*)SKINOFFSETTOPTR(skin_buffer, token->value.data);

        if (img && !img->loaded)
        {
            if (img->using_preloaded_icons)
            {
                img->loaded = true;
                token->type = SKIN_TOKEN_IMAGE_DISPLAY_LISTICON;
            }
            else
            {
                char path[MAX_PATH];
                int handle;
                strmemccpy(path, img->bm.data, sizeof(path));
                handle = load_skin_bmp(wps_data, img, bmpdir);
                img->buflib_handle = handle;
                img->loaded = img->buflib_handle > 0;

                if (img->loaded)
                {
                    struct skin_token_list *imglist = SKINOFFSETTOPTR(skin_buffer, list->next);
                    img->subimage_height = img->bm.height / img->num_subimages;
                    struct bitmap* loaded_bm = &img->bm;
                    while (imglist)
                    {
                        token = SKINOFFSETTOPTR(skin_buffer, imglist->token);
                        if (token) {
                            img = (struct gui_img*)SKINOFFSETTOPTR(skin_buffer, token->value.data);
                            if (img && img->bm.data && !strcmp(path, img->bm.data))
                            {
                                img->loaded = true;
                                img->buflib_handle = handle;
                                img->bm = *loaded_bm;
                                img->subimage_height = img->bm.height / img->num_subimages;
                            }
                        }
                        imglist = SKINOFFSETTOPTR(skin_buffer, imglist->next);
                    }
                }
                else
                    retval = false;
            }
        }
    skip:
        list = SKINOFFSETTOPTR(skin_buffer, list->next);
    }

    wps_data->backdrop_id = skin_backdrop_assign(backdrop_filename, bmpdir, curr_screen);
    return retval;
}

static bool skin_load_fonts(struct wps_data *data)
{
    /* don't spit out after the first failue to aid debugging */
    int16_t id_array[MAXUSERFONTS];
    int font_count = 0;
    bool success = true;
    struct skin_element *vp_list;
    int font_id;
    /* walk though each viewport and assign its font */
    for(vp_list = SKINOFFSETTOPTR(skin_buffer, data->tree);
        vp_list; vp_list = SKINOFFSETTOPTR(skin_buffer, vp_list->next))
    {
        /* first, find the viewports that have a non-sys/ui-font font */
        struct skin_viewport *skin_vp =
                SKINOFFSETTOPTR(skin_buffer, vp_list->data);
        if (!skin_vp) continue;
        struct viewport *vp = &skin_vp->vp;

        font_id = skin_vp->parsed_fontid;
        if (font_id == 1)
        {   /* the usual case -> built-in fonts */
            vp->font = screens[curr_screen].getuifont();
            continue;
        }
        else if (font_id <= 0)
        {
            vp->font = FONT_SYSFIXED;
            DEBUGF("WARNING: Do not use SYSFONT (id 0) in viewports!\n");
            continue;
        }

        /* now find the corresponding skin_font */
        struct skin_font *font = &skinfonts[font_id-2];
        if (!font->name)
        {
            if (success)
            {
                splashf(HZ, ID2P(LANG_FONT_LOAD_ERROR), "(no name)");
                DEBUGF("font %d not specified\n", font_id);
            }
            success = false;
            continue;
        }

        /* load the font - will handle loading the same font again if
         * multiple viewports use the same */
        if (font->id < 0)
        {
            char path[MAX_PATH];
            snprintf(path, sizeof path, FONT_DIR "/%s", font->name);
            font->id = font_load_ex(path, 0, skinfonts[font_id-2].glyphs);

            /* printf("[%d] %s -> %d\n",font_id, font->name, font->id); */
            id_array[font_count++] = font->id;
        }

        if (font->id < 0)
        {
            splashf(HZ, ID2P(LANG_FONT_LOAD_ERROR), font->name);
            DEBUGF("Unable to load font %d: '%s'\n", font_id, font->name);
            font->name = NULL; /* to stop trying to load it again if we fail */
            success = false;
            continue;
        }

        /* finally, assign the font_id to the viewport */
        vp->font = font->id;
    }
    if (font_count)
    {
        int16_t *font_ids = skin_buffer_alloc(font_count * sizeof(font_ids[0]));
        if (!success || font_ids == NULL)
        {
            while (font_count > 0)
            {
                if(id_array[--font_count] != -1)
                    font_unload(id_array[font_count]);
            }
            data->font_ids = PTRTOSKINOFFSET(skin_buffer, NULL);
            return false;
        }
        memcpy(font_ids, id_array, sizeof(font_ids[0])*font_count);
        data->font_count = font_count;
        data->font_ids = PTRTOSKINOFFSET(skin_buffer, font_ids);
    }
    else
    {
        data->font_count = 0;
        data->font_ids = PTRTOSKINOFFSET(skin_buffer, NULL);
    }
    return success;
}

static int convert_viewport(struct wps_data *data, struct skin_element* element)
{
    struct skin_viewport *skin_vp = skin_buffer_alloc(sizeof(*skin_vp));
    struct screen *display = &screens[curr_screen];

    if (!skin_vp)
        return CALLBACK_ERROR;

    skin_vp->hidden_flags = 0;
    skin_vp->label = PTRTOSKINOFFSET(skin_buffer, NULL);
    skin_vp->is_infovp = false;
    skin_vp->parsed_fontid = 1;
    element->data = PTRTOSKINOFFSET(skin_buffer, skin_vp);
    curr_vp = skin_vp;
    curr_viewport_element = element;
    if (!first_viewport)
        first_viewport = element;

    viewport_set_defaults(&skin_vp->vp, curr_screen);

    skin_vp->output_to_backdrop_buffer = false;
    skin_vp->start_gradient.start = global_settings.lss_color;
    skin_vp->start_gradient.end = global_settings.lse_color;
    skin_vp->start_gradient.text = global_settings.lst_color;
    skin_vp->dc_orig_fg = skin_vp->vp.fg_pattern;
    skin_vp->dc_orig_bg = skin_vp->vp.bg_pattern;


    struct skin_tag_parameter *param = get_param(element, 0);
    if (element->params_count == 0) /* default viewport */
    {
        if (data->tree < 0) /* first viewport in the skin */
            data->tree = PTRTOSKINOFFSET(skin_buffer, element);
        skin_vp->label = VP_DEFAULT_LABEL;
        return CALLBACK_OK;
    }

    if (element->params_count == 6)
    {
        if (element->tag->type == SKIN_TOKEN_UIVIEWPORT_LOAD)
        {
            skin_vp->is_infovp = true;
            if (isdefault(param))
            {
                skin_vp->hidden_flags = VP_NEVER_VISIBLE;
                skin_vp->label = VP_DEFAULT_LABEL;
            }
            else
            {
                skin_vp->hidden_flags = VP_NEVER_VISIBLE;
                skin_vp->label = param->data.text;
            }
        }
        else
        {
                skin_vp->hidden_flags = VP_DRAW_HIDEABLE|VP_DRAW_HIDDEN;
                skin_vp->label = param->data.text;
        }
        param++;
    }
    /* x */
    if (!isdefault(param))
    {
        skin_vp->vp.x = param->data.number;
        if (param->data.number < 0)
            skin_vp->vp.x += display->lcdwidth;
        else if (param->type == PERCENT)
            skin_vp->vp.x = param->data.number * display->lcdwidth / 1000;
    }
    param++;
    /* y */
    if (!isdefault(param))
    {
        skin_vp->vp.y = param->data.number;
        if (param->data.number < 0)
            skin_vp->vp.y += display->lcdheight;
        else if (param->type == PERCENT)
            skin_vp->vp.y = param->data.number * display->lcdheight / 1000;
    }
    param++;
    /* width */
    if (!isdefault(param))
    {
        skin_vp->vp.width = param->data.number;
        if (param->data.number < 0)
            skin_vp->vp.width = (skin_vp->vp.width + display->lcdwidth) - skin_vp->vp.x;
        else if (param->type == PERCENT)
            skin_vp->vp.width = param->data.number * display->lcdwidth / 1000;
    }
    else
    {
        skin_vp->vp.width = display->lcdwidth - skin_vp->vp.x;
    }
    param++;
    /* height */
    if (!isdefault(param))
    {
        skin_vp->vp.height = param->data.number;
        if (param->data.number < 0)
            skin_vp->vp.height = (skin_vp->vp.height + display->lcdheight) - skin_vp->vp.y;
        else if (param->type == PERCENT)
            skin_vp->vp.height = param->data.number * display->lcdheight / 1000;
    }
    else
    {
        skin_vp->vp.height = display->lcdheight - skin_vp->vp.y;
    }
    param++;
    /* font */
    if (!isdefault(param))
        skin_vp->parsed_fontid = param->data.number;
    if ((unsigned) skin_vp->vp.x >= (unsigned) display->lcdwidth ||
        skin_vp->vp.width + skin_vp->vp.x > display->lcdwidth ||
        (unsigned) skin_vp->vp.y >= (unsigned) display->lcdheight ||
        skin_vp->vp.height + skin_vp->vp.y > display->lcdheight)
        return CALLBACK_ERROR;

    /* Fix x position for RTL languages */
    if (follow_lang_direction && lang_is_rtl())
        skin_vp->vp.x = display->lcdwidth - skin_vp->vp.x - skin_vp->vp.width;

    return CALLBACK_OK;
}

static int skin_element_callback(struct skin_element* element, void* data)
{
    struct wps_data *wps_data = (struct wps_data *)data;
    struct wps_token *token;
    parse_function function = NULL;

    switch (element->type)
    {
        /* IMPORTANT: element params are shared, so copy them if needed
         *            or use then NOW, dont presume they have a long lifespan
         */
        case TAG:
        {
            token = skin_buffer_alloc(sizeof(*token));
            memset(token, 0, sizeof(*token));
            token->type = element->tag->type;
            token->value.data = INVALID_OFFSET;

            if (element->tag->flags&SKIN_RTC_REFRESH)
            {
                curr_line->update_mode |= SKIN_REFRESH_DYNAMIC;
            }
            else
                curr_line->update_mode |= element->tag->flags&SKIN_REFRESH_ALL;

            element->data = PTRTOSKINOFFSET(skin_buffer, token);

            /* Some tags need special handling for the tag, so add them here */
            switch (token->type)
            {
                case SKIN_TOKEN_ALIGN_LANGDIRECTION:
                    follow_lang_direction = 2;
                    break;
                case SKIN_TOKEN_LOGICAL_IF:
                    function = parse_logical_if;
                    break;
                case SKIN_TOKEN_LOGICAL_AND:
                case SKIN_TOKEN_LOGICAL_OR:
                    function = parse_logical_andor;
                    break;
                case SKIN_TOKEN_SUBSTRING:
                    function = parse_substring_tag;
                    break;
                case SKIN_TOKEN_SELECT:
                    function = parse_select;
                    break;
                case SKIN_TOKEN_TEXT_WIDTH:
                    function = parse_store_element;
                    break;
                case SKIN_TOKEN_WORD_WRAP:
                    function = parse_wordwrap;
                    break;
                case SKIN_TOKEN_MATH:
                case SKIN_TOKEN_STRLEN:
                case SKIN_TOKEN_STRFIND:
                case SKIN_TOKEN_PAD:
                    function = parse_store_element;
                    break;
                case SKIN_TOKEN_TEXT_BOX:
                    function = parse_textbox;
                    break;
                case SKIN_TOKEN_PROGRESSBAR:
                case SKIN_TOKEN_VOLUME:
                case SKIN_TOKEN_BATTERY_PERCENT:
                case SKIN_TOKEN_PLAYER_PROGRESSBAR:
                case SKIN_TOKEN_PEAKMETER_LEFT:
                case SKIN_TOKEN_PEAKMETER_RIGHT:
                case SKIN_TOKEN_PLAYLIST_PERCENT:
                case SKIN_TOKEN_PLAYLIST_ELAPSED_PERCENT:
                case SKIN_TOKEN_LIST_NEEDS_SCROLLBAR:
                    function = parse_progressbar_tag;
                    break;
                case SKIN_TOKEN_SUBLINE_TIMEOUT:
                case SKIN_TOKEN_BUTTON_VOLUME:
                case SKIN_TOKEN_TRACK_STARTING:
                case SKIN_TOKEN_TRACK_ENDING:
                    function = parse_timeout_tag;
                    break;
                case SKIN_TOKEN_LIST_ITEM_TEXT:
                case SKIN_TOKEN_LIST_ITEM_ICON:
                case SKIN_TOKEN_LIST_ITEM_ALBUMART:
                    function = parse_listitem;
                    break;
                case SKIN_TOKEN_SPECTRUM_BARS:
                    function = parse_spectrumbars;
                    break;
                case SKIN_TOKEN_DISABLE_THEME:
                case SKIN_TOKEN_ENABLE_THEME:
                case SKIN_TOKEN_DRAW_INBUILTBAR:
                    function = parse_statusbar_tags;
                    break;
                case SKIN_TOKEN_LIST_TITLE_TEXT:
                    sb_skin_has_title(curr_screen);
                    break;
                case SKIN_TOKEN_TOP_QUICKSETTING_NAME:
                case SKIN_TOKEN_TOP_QUICKSETTING_VALUE:
                case SKIN_TOKEN_BOTTOM_QUICKSETTING_NAME:
                case SKIN_TOKEN_BOTTOM_QUICKSETTING_VALUE:
                case SKIN_TOKEN_LEFT_QUICKSETTING_NAME:
                case SKIN_TOKEN_LEFT_QUICKSETTING_VALUE:
                case SKIN_TOKEN_RIGHT_QUICKSETTING_NAME:
                case SKIN_TOKEN_RIGHT_QUICKSETTING_VALUE:
                    wps_data->draws_quickscreen = true;
                    break;
                case SKIN_TOKEN_DRAWRECTANGLE:
                    function = parse_drawrectangle;
                    break;
                case SKIN_TOKEN_FILE_DIRECTORY:
                    token->value.i = get_param(element, 0)->data.number;
                    break;
                case SKIN_TOKEN_FILE_TEXT:
                    function = parse_filetext;
                    break;
                case SKIN_TOKEN_VIEWPORT_FGCOLOUR:
                case SKIN_TOKEN_VIEWPORT_BGCOLOUR:
                    function = parse_viewportcolour;
                    break;
                case SKIN_TOKEN_IMAGE_BACKDROP:
                    function = parse_image_special;
                    break;
                case SKIN_TOKEN_VIEWPORT_TEXTSTYLE:
                    function = parse_viewporttextstyle;
                    break;
                case SKIN_TOKEN_VIEWPORT_DRAWONBG:
                    curr_vp->output_to_backdrop_buffer = true;
                    backdrop_filename = BACKDROP_BUFFERNAME;
                    wps_data->use_extra_framebuffer = true;
                    break;
                case SKIN_TOKEN_VIEWPORT_GRADIENT_SETUP:
                    function = parse_viewport_gradient_setup;
                    break;
                case SKIN_TOKEN_TRANSLATEDSTRING:
                case SKIN_TOKEN_SETTING:
                    function = parse_setting_and_lang;
                    break;
                case SKIN_TOKEN_VIEWPORT_CUSTOMLIST:
                    function = parse_playlistview;
                    break;
                case SKIN_TOKEN_LOAD_FONT:
                    function = parse_font_load;
                    break;
                case SKIN_TOKEN_VIEWPORT_ENABLE:
                case SKIN_TOKEN_UIVIEWPORT_ENABLE:
                    token->value.data = get_param(element, 0)->data.text;
                    break;
                case SKIN_TOKEN_IMAGE_PRELOAD_DISPLAY:
                case SKIN_TOKEN_IMAGE_DISPLAY_9SEGMENT:
                    function = parse_image_display;
                    break;
                case SKIN_TOKEN_IMAGE_PRELOAD:
                case SKIN_TOKEN_IMAGE_DISPLAY:
                    function = parse_image_load;
                    break;
                case SKIN_TOKEN_LIST_ITEM_CFG:
                    function = parse_listitemviewport;
                    break;
                case SKIN_TOKEN_ALBUMART_LOAD:
                    function = parse_albumart_load;
                    break;
                case SKIN_TOKEN_ALBUMART_DISPLAY:
                    function = parse_albumart_display;
                    break;
                case SKIN_TOKEN_VAR_SET:
                case SKIN_TOKEN_VAR_GETVAL:
                case SKIN_TOKEN_VAR_TIMEOUT:
                    function = parse_skinvar;
                    break;
                default:
                    break;
            }
            if (function)
            {
                if (function(element, token, wps_data) != 0)
                    return CALLBACK_ERROR;
            }
            /* tags that start with 'F', 'I' or 'D' are for the next file */
            if ( *(element->tag->name) == 'I' || *(element->tag->name) == 'F' ||
                 *(element->tag->name) == 'D')
                token->next = true;
            else if ( token->type == SKIN_TOKEN_TRACK_LENGTH && *(element->tag->name) == 'P')
                token->next = true;

            if (follow_lang_direction > 0 )
                follow_lang_direction--;
            break;
        }
        case VIEWPORT:
            return convert_viewport(wps_data, element);
        case LINE:
        {
            curr_line = skin_buffer_alloc(sizeof(*curr_line));
            curr_line->update_mode = SKIN_REFRESH_STATIC;
            element->data = PTRTOSKINOFFSET(skin_buffer, curr_line);
        }
        break;
        case LINE_ALTERNATOR:
        {
            struct line_alternator *alternator = skin_buffer_alloc(sizeof(*alternator));
            alternator->current_line = 0;
            alternator->next_change_tick = current_tick;
            element->data = PTRTOSKINOFFSET(skin_buffer, alternator);
        }
        break;
        case CONDITIONAL:
        {
            struct conditional *conditional = skin_buffer_alloc(sizeof(*conditional));
            conditional->last_value = -1;
            conditional->token = element->data;
            element->data = PTRTOSKINOFFSET(skin_buffer, conditional);
            if (!check_feature_tag(element->tag->type))
            {
                return FEATURE_NOT_AVAILABLE;
            }
            return CALLBACK_OK;
        }
        case TEXT:
            curr_line->update_mode |= SKIN_REFRESH_STATIC;
            break;
        default:
            break;
    }
    return CALLBACK_OK;
}

/* to setup up the wps-data from a format-buffer (isfile = false)
   from a (wps-)file (isfile = true)*/
bool skin_data_load(enum screen_type screen, struct wps_data *wps_data,
                    const char *buf, bool isfile, struct skin_stats *stats)
{
    char *wps_buffer = NULL;
    if (!wps_data || !buf)
        return false;
    int i;
    for (i=0;i<MAXUSERFONTS;i++)
    {
        skinfonts[i].id = -1;
        skinfonts[i].name = NULL;
    }
#ifdef DEBUG_SKIN_ENGINE
    if (isfile && debug_wps)
    {
        DEBUGF("\n=====================\nLoading '%s'\n=====================\n", buf);
    }
#endif

    _stats = stats;
    skin_clear_stats(stats);
    /* get buffer space from the plugin buffer */
    size_t buffersize = 0;
    wps_buffer = (char *)app_get_buffer(&buffersize, "skin parser");

    if (!wps_buffer)
        return false;

    skin_data_reset(wps_data);
    wps_data->wps_loaded = false;
    curr_screen = screen;
    curr_line = NULL;
    curr_vp = NULL;
    curr_viewport_element = NULL;
    first_viewport = NULL;
    curr_albumart = NULL;

    if (isfile)
    {
        int fd = open_utf8(buf, O_RDONLY);

        if (fd < 0)
            return false;
        /* copy the file's content to the buffer for parsing,
           ensuring that every line ends with a newline char. */
        unsigned int start = 0;
        while(read_line(fd, wps_buffer + start, buffersize - start) > 0)
        {
            start += strlen(wps_buffer + start);
            if (start < buffersize - 1)
            {
                wps_buffer[start++] = '\n';
                wps_buffer[start] = 0;
            }
        }
        close(fd);
        if (start <= 0)
            return false;
        start++;
        skin_buffer = &wps_buffer[start];
        buffersize -= start;
    }
    else
    {
        skin_buffer = wps_buffer;
        wps_buffer = (char*)buf;
    }

    /* align to long */
    ALIGN_BUFFER(skin_buffer, buffersize, sizeof(long));
    backdrop_filename = "-";
    wps_data->backdrop_id = -1;
    /* parse the skin source */
    skin_buffer_init(skin_buffer, buffersize);
    struct skin_element *tree = skin_parse(wps_buffer, skin_element_callback, wps_data);
    wps_data->tree = PTRTOSKINOFFSET(skin_buffer, tree);
    if (!SKINOFFSETTOPTR(skin_buffer, wps_data->tree)) {
#ifdef DEBUG_SKIN_ENGINE
        if (isfile && debug_wps)
            skin_error_format_message();
#endif
        skin_data_reset(wps_data);
        return false;
    }

    char bmpdir[MAX_PATH];
    if (isfile)
    {
        /* get the bitmap dir */
        const char *dot = strrchr(buf, '.');
        strmemccpy(bmpdir, buf, dot - buf + 1);
    }
    else
    {
        snprintf(bmpdir, MAX_PATH, "%s", BACKDROP_DIR);
    }
    /* load the bitmaps that were found by the parsing */
    if (!load_skin_bitmaps(wps_data, bmpdir) ||
        !skin_load_fonts(wps_data))
    {
        skin_data_reset(wps_data);
        return false;
    }
    /* The skin wants album art, so the sizes playback buffers have just
     * changed -- have it rebuffer against the slots now claimed. */
    if (skin_albumart_first(skin_buffer, wps_data->albumart))
        playback_update_aa_dims();
    wps_data->buflib_handle = core_alloc(skin_buffer_usage());
    if (wps_data->buflib_handle > 0)
    {
        /* Pin the skin buffer so it never moves. list-skinned.c (listcfg) and
         * skin viewports cache raw pointers into this buffer; if background
         * buflib compaction (e.g. a tagcache commit or the album-art
         * thumbnail cache) relocated it, those pointers would dangle and
         * skinlist_draw() would take a data abort. */
        core_pin(wps_data->buflib_handle);
        wps_data->wps_loaded = true;
        memcpy(core_get_data(wps_data->buflib_handle), skin_buffer,
                skin_buffer_usage());
        stats->buflib_handles++;
        stats->browser_size = skin_buffer_usage();

        /* A blurring %Cl draws from a buffer of its own rather than from the
         * buffered art, so give it one -- now, at skin load, and not at the
         * track change that first needs it. A core_alloc from a screen
         * shrinks the audio buffer and forces a rebuffer; doing that on every
         * track would stutter playback. Backdrops allocate here for the same
         * reason.
         *
         * The bounding box is an upper bound: the art is fitted inside it
         * keeping its aspect, so square art in a full-screen box uses 240x240
         * of the 320x240 asked for. */
        char *live_buffer = get_skin_buffer(wps_data);
        struct skin_token_list *node =
                SKINOFFSETTOPTR(live_buffer, wps_data->albumart);

        while (node)
        {
            struct skin_albumart *live = skin_albumart_of(live_buffer, node);

            if (live && (live->filter.stages & IMG_CLASS_RESIZE))
            {
                size_t bytes = (size_t)live->width * live->height
                             * sizeof(fb_data);

                live->filter_handle = core_alloc(bytes);
                if (live->filter_handle <= 0)
                    DEBUGF("%%Cl: no room for a %zu byte filter buffer\n", bytes);
                else
                    stats->buflib_handles++;
            }
            node = SKINOFFSETTOPTR(live_buffer, node->next);
        }
    }
    else
    {
        /* No buflib copy, so the chain the slots are recorded in dies with the
         * parse buffer this function is about to let go of -- and
         * skin_data_free_buflib_allocs() takes its `!skin_buffer` exit, having
         * nothing left to walk. Give them back here or they are gone until
         * reboot, which with two slots means album art stops working.
         *
         * The skin itself is already inert: get_skin_buffer() returns NULL
         * without a handle and every renderer turns round on that. */
        struct skin_token_list *node =
                SKINOFFSETTOPTR(skin_buffer, wps_data->albumart);

        while (node)
        {
            struct skin_albumart *aa = skin_albumart_of(skin_buffer, node);

            if (aa)
                playback_release_aa_slot(aa->slot);
            node = SKINOFFSETTOPTR(skin_buffer, node->next);
        }
        wps_data->albumart = PTRTOSKINOFFSET(skin_buffer, NULL);
    }

    skin_buffer = NULL;
    return true;
}
