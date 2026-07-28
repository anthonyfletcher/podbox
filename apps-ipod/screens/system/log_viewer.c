/***************************************************************************
 * Original code from RockBox
 * was: apps/logfdisp.c
 * Copyright (C) 2005 Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * On-device viewer and dumper for the circular logf() buffer. Only built
 * when ROCKBOX_HAS_LOGF is set.
 ****************************************************************************/
#include "config.h"

#ifdef ROCKBOX_HAS_LOGF
#include <file.h>
#include <timefuncs.h>
#include <string.h>
#include <kernel.h>
#include "rbpaths.h"
#include "input/action.h"

#include <lcd.h>
#include <font.h>
#include "widgets/menu.h"
#include "logf.h"
#include "settings/settings.h"
#include "log_viewer.h"
#include "input/action.h"
#include "widgets/splash.h"
#include "system/strutil.h"
int compute_nb_lines(int w, struct font* font)
{
    int i, nb_lines;
    int cur_x, delta_x;

    if(logfindex == 0 && !logfwrap)
        return 0;

    if(logfwrap)
        i = logfindex;
    else
        i = 0;
    
    cur_x = 0;
    nb_lines = 0;
        
    do {
        if(logfbuffer[i] == '\0')
        {
            cur_x = 0;
            nb_lines++;
        }
        else
        {
            /* does character fit on this line ? */
            delta_x = font_get_width(font, logfbuffer[i]);
            
            if(cur_x + delta_x > w)
            {
                cur_x = 0;
                nb_lines++;
            }
            
            /* update pointer */
            cur_x += delta_x;
        }

        i++;
        if(i >= MAX_LOGF_SIZE)
            i = 0;
    } while(i != logfindex);
    
    return nb_lines;
}

bool log_viewer_show(void)
{
    int action;
    int w, h, i, index;
    int fontnr;
    int cur_x, cur_y, delta_y, delta_x;
    struct font* font;
    int user_index;/* user_index will be number of the first line to display (warning: line!=logf entry) */
    char buf[2];
    
    fontnr = lcd_getfont();
    font = font_get(fontnr);
    
    /* get the horizontal size of each line */
    font_getstringsize("A", NULL, &delta_y, fontnr);
    
    buf[1] = '\0';
    w = LCD_WIDTH;
    h = LCD_HEIGHT;
    /* start at the end of the log */
    user_index = compute_nb_lines(w, font) - h/delta_y -1; /* if negative, will be set 0 to zero later */

    do {
        lcd_clear_display();
        
        if(user_index < 0)
            user_index = 0;
        
        if(logfwrap)
            i = logfindex;
        else
            i = 0;
        
        index = 0;
        cur_x = 0;
        cur_y = 0;
        
        /* nothing to print ? */
        if(logfindex == 0 && !logfwrap)
            goto end_print;
        
        do {
            if(logfbuffer[i] == '\0')
            {
                /* should be display a newline ? */
                if(index >= user_index)
                    cur_y += delta_y;
                cur_x = 0;
                index++;
            }
            else
            {
                /* does character fit on this line ? */
                delta_x = font_get_width(font, logfbuffer[i]);
                
                if(cur_x + delta_x > w)
                {
                    /* should be display a newline ? */
                    if(index >= user_index)
                        cur_y += delta_y;
                    cur_x = 0;
                    index++;
                }
                
                /* should we print character ? */
                if(index >= user_index)
                {
                    buf[0] = logfbuffer[i];
                    lcd_putsxy(cur_x, cur_y, buf);
                }
                
                /* update pointer */
                cur_x += delta_x;
            }
            
            /* did we fill the screen ? */
            if(cur_y > h)
                break;
            
            i++;
            if(i >= MAX_LOGF_SIZE)
                i = 0;
        } while(i != logfindex);
        
        end_print:
        lcd_update();
        
        action = get_action(CONTEXT_STD, HZ);
        switch( action )
        {
            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                user_index++;
                break;
            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                user_index--;
                break;
            case ACTION_STD_OK:
                user_index = 0;
                break;
            default:
                break;
        }
    } while(action != ACTION_STD_CANCEL);

    return false;
}

bool log_viewer_dump(void)
{
    int fd;

    splashf(HZ, "Log File Dumped");

    /* nothing to print ? */
    if(logfindex == 0 && !logfwrap)
        /* nothing is logged just yet */
        return false;

    logfenabled = false;

    char fname[MAX_PATH];
    struct tm *nowtm = get_time();
    fd = open_pathfmt(fname, sizeof(fname), O_CREAT|O_WRONLY|O_TRUNC,
             "%s/logf_%04d%02d%02d%02d%02d%02d.txt", ROCKBOX_DIR,
             nowtm->tm_year + 1900, nowtm->tm_mon + 1, nowtm->tm_mday,
             nowtm->tm_hour, nowtm->tm_min, nowtm->tm_sec);
    if(-1 != fd) {
        int i;

        if(logfwrap)
            i = logfindex;
        else
            i = 0;

        do {
            if(logfbuffer[i]=='\0')
                fdprintf(fd, "\n");
            else
                fdprintf(fd, "%c", logfbuffer[i]);

            i++;
            if(i >= MAX_LOGF_SIZE)
                i = 0;
        } while(i != logfindex);

        close(fd);
    }

    logfenabled = true;

    return false;
}

#endif /* ROCKBOX_HAS_LOGF */
