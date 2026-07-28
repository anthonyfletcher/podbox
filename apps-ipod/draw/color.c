/***************************************************************************
 * Original code from RockBox
 * was: apps/misc.c (colour parsing)
 * GNU General Public License (version 2+)
 *
 * Parses colours from text: "#rrggbb" hex into a native pixel value, and the
 * named/hex forms skins and theme files use.
 ****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "lcd.h"
#include "settings/settings.h"
#include "draw/screen_access.h"
#include "color.h"

/*
 * Helper function to convert a string of 6 hex digits to a native colour
 */
static int hex2dec(int c)
{
    return  (((c) >= '0' && ((c) <= '9')) ? (c) - '0' :
                                            (toupper(c)) - 'A' + 10);
}

int hex_to_rgb(const char* hex, int* color)
{
    int red, green, blue;
    int i = 0;

    while ((i < 6) && (isxdigit(hex[i])))
        i++;

    if (i < 6)
        return -1;

    red = (hex2dec(hex[0]) << 4) | hex2dec(hex[1]);
    green = (hex2dec(hex[2]) << 4) | hex2dec(hex[3]);
    blue = (hex2dec(hex[4]) << 4) | hex2dec(hex[5]);

    *color = LCD_RGBPACK(red,green,blue);

    return 0;
}

/* '0'-'3' are ASCII 0x30 to 0x33 */
#define is0123(x) (((x) & 0xfc) == 0x30)
bool parse_color(enum screen_type screen, char *text, int *value)
{
    (void)text; (void)value; /* silence warnings on mono bitmap */
    (void)screen;

    if (screens[screen].depth > 2)
    {
        if (hex_to_rgb(text, value) < 0)
            return false;
        else
            return true;
    }

    return false;
}
