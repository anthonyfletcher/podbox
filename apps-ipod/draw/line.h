/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/line.h
 * Copyright (C) 2013 Thomas Martitz
 * GNU General Public License (version 2+)
 *
 * Interface to line.c: the line_desc style descriptor and
 * put_line()/vput_line().
 ****************************************************************************/

#ifndef __LINE_H__
#define __LINE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include "lcd.h"

/* put_line()/vput_line() take a screen by pointer only, so a forward
 * declaration is enough and this header need not pull in screens.h. */
struct screen;

/* Possible line decoration styles. Specify one of
 * STYLE_NONE, _DEFAULT, _INVERT, _COLORBAR or _GRADIENT, and optionally
 * or with STYLE_COLORED specifying line_desc.text_color */
enum line_styles {
    /* Just print the text. Do not clear or draw line decorations */
    STYLE_NONE       = 0x00,
    /* Line background filled with the bg color (or backdrop if present) */ 
    STYLE_DEFAULT    = 0x01,
    /* Like STYLE_DFEAULT except that text and background color will be swapped */
    STYLE_INVERT     = 0x02,
    /* Line background filled with line color line_desc.line_color */
    STYLE_COLORBAR   = 0x04,
    /* Line background filled with gradient, colors taken from
     * line_desc.line_color and line_desc.line_end_color */
    STYLE_GRADIENT   = 0x08,
    /* Modifier for the text color, which will be taken from line_desc.text_color */
    STYLE_COLORED    = 0x10,
    /* Modifier: draw a drop shadow behind the text, from the shadow_ fields */
    STYLE_SHADOW     = 0x20,
    /* These are used internally */
    _STYLE_DECO_MASK = 0x0f,
    _STYLE_MODE_MASK = 0x7F,
};

/* Ceilings on a drop shadow, bounding the scratch it is drawn in --
 * see draw/text_shadow.c. */
#define TEXT_SHADOW_MAX_OFFSET 8
#define TEXT_SHADOW_MAX_BLUR   3

struct line_desc {
    /* height of the line (in pixels). -1 to inherit the height
     * from the font. The text will be centered if the height is larger,
     * but the decorations will span the entire height */
    int height;
    /* multiline support: For some decorations (e.g. gradient) to work
     * across multiple lines (e.g. to draw a line selector across 2 lines)
     * the line index and line count must be known. For normal, single
     * lines specify nlines=1 and line=0 */
    /* line count of a group */
    int16_t nlines;
    /* index of the line in the group */
    int16_t line;
    /* line text color if STYLE_COLORED is specified, in native
     * lcd format (convert with LCD_RGBPACK() if necessary) */
    unsigned text_color;
    /* line color if STYLE_COLORBAR or STYLE_GRADIENT is specified, in native
     * lcd format (convert with LCD_RGBPACK() if necessary) */
    unsigned line_color, line_end_color;
    /* drop shadow if STYLE_SHADOW is specified: colour in native lcd format,
     * offset from the text in pixels, blur radius in pixels (0 for a hard
     * edge) and opacity, 0 to LCD_BLEND_OPAQUE */
    unsigned shadow_color;
    int8_t shadow_x, shadow_y, shadow_blur, shadow_opacity;
    /* line decorations, see STYLE_DEFAULT etc. */
    enum line_styles style;
    /* whether the line can scroll */
    bool scroll;
    /* height of the line separator (in pixels). 0 to disable drawing
     * of the separator */
    int8_t separator_height;
};

/* default initializer, can be used for static initialitation also.
 * This initializer will result in single lines without style that don't scroll */
#define LINE_DESC_DEFINIT { .style = STYLE_DEFAULT, .height = -1, .separator_height = 0, .line = 0, .nlines = 1, .scroll = false }

/**
 * Print a line at a given pixel postion, using decoration information from
 * line and content information from the format specifier. The format specifier
 * can include tags that depend on further parameters given to the function
 * (similar to the well-known printf()).
 *
 * Tags start with the $ sign. Below is a list of the currently supported tags:
 * $s - insert a column (1px wide) of empty space.
 * $S - insert a column (1 char wide) of empty space.
 * $i - insert an icon. put_line() expects a corresponding parameter of the
 *      type 'enum themable_icons'. If Icon_NOICON is passed, then empty
 *      space (icon-wide) will be inserted.
 * $I - insert an icon with padding. Works like $i but additionally
 *      adds a column (1px wide) of empty space on either side of the icon.
 * $t - insert text. put_line() expects a corresponding parameter of the type
 *      'const char *'
 * $$ - insert a '$' char, use it to escape $.
 *
 * $I, $s and $S support the following two forms:
 * $n[IsS] - inserts n columns (pixels/chars respectively) of empty space
 * $*[IsS] - inserts n columns (pixels/chars respectively) of empty space. put_line()
 *           expects a correspinding paramter of the type 'int' that specifies n.
 *
 * $t supports the following two forms:
 * $nt     - skips the first n pixels when displaying the string
 * $*t     - skips the first n pixels when displaying the string. put_line()
 *           expects a correspinding paramter of the type 'int' that specifies n.
 *
 * Inline text will be printed as is (be sure to escape '$') and can be freely
 * intermixed with tags. Inline text will be truncated after MAX_PATH+31 bytes.
 * If you have a longer inline string use a separate buffer and pass that via $t,
 * which does not suffer from this truncation.
 * 
 * Text can appear anywhere, before or after (or both) tags. However, when the
 * line can scroll, only the last piece of text (whether inline or via $t) can
 * scroll. This is due to a scroll_engine limitation.
 *
 * x, y - pixel postion of the line.
 * line - holds information for the line decorations
 * fmt  - holds the text and optionally format tags
 * ...  - additional paramters for the format tags
 *
 */
void  put_line(struct screen *display,
               int x, int y, struct line_desc *line,
               const char *fmt, ...);


/**
 * Print a line at a given pixel postion, using decoration information from
 * line and content information from the format specifier. The format specifier
 * can include tags that depend on further parameters given to the function
 * (similar to the well-known vprintf()).
 *
 * For details, see put_line(). This function is equivalent, except for
 * accepting a va_list instead of a variable paramter list.
 */
void vput_line(struct screen *display,
               int x, int y, struct line_desc *line,
               const char *fmt, va_list ap);

#endif /* __LINE_H__*/
