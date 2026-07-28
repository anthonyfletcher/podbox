/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/icons.h
 * Copyright (C) 2002 Robert E. Hak
 * GNU General Public License (version 2+)
 *
 * Declares the built-in icon bitmap tables and the core bitmap registry
 * entry type.
 ****************************************************************************/
#ifndef _ICON_BITMAPS_H_
#define _ICON_BITMAPS_H_


#include <lcd.h>
#include "metadata.h"


struct cbmp_bitmap_info_entry /* */
{
    const unsigned char* pbmp;
    unsigned char width;
    unsigned char height; /* !ASSUMES MULTIPLES OF 8! */
    unsigned char count;
};

enum cbmp_bitmap_format
{
    CBMP_Mono_5x8 = 0,
    CBMP_Mono_7x8,
    CBMP_Mono_12x8,
    CBMP_BitmapFormatLast
};

extern const struct cbmp_bitmap_info_entry core_bitmaps[CBMP_BitmapFormatLast];

/* Symbolic names for icons */
enum icons_5x8 {
    Icon_Lock_Main,
    Icon_Lock_Remote,
    Icon_Stereo,
    Icon_Mono,
    Icon5x8Last
};

enum icons_7x8 {
    Icon_Plug,
    Icon_USBPlug,
    Icon_Mute,
    Icon_Play,
    Icon_Stop,
    Icon_Pause,
    Icon_FastForward,
    Icon_FastBackward,
    Icon_Record,
    Icon_RecPause,
    Icon_Radio,
    Icon_Radio_Mute,
    Icon_Repeat,
    Icon_RepeatOne,
    Icon_Shuffle,
    Icon_DownArrow,
    Icon_UpArrow,
    Icon_RepeatAB,
    Icon7x8Last
};

enum icons_12x8 {
    Icon_Disk,
    Icon12x8Last
};


extern const unsigned char bitmap_icons_5x8[Icon5x8Last][5];
extern const unsigned char bitmap_icons_7x8[Icon7x8Last][7];
extern const unsigned char bitmap_icon_disk[];

#define STATUSBAR_X_POS       0
#define STATUSBAR_Y_POS       0 /* MUST be a multiple of 8 */
#define STATUSBAR_HEIGHT      SYSFONT_HEIGHT
#define STATUSBAR_WIDTH       LCD_WIDTH
#define SB_ICON_HEIGHT        8 /* ... for now */
#define ICON_BATTERY_X_POS    0
#define ICON_BATTERY_WIDTH    (2+(2*SYSFONT_WIDTH))
#define ICON_PLUG_X_POS       STATUSBAR_X_POS+ICON_BATTERY_WIDTH+2
#define ICON_PLUG_WIDTH       7
#define ICON_VOLUME_X_POS     STATUSBAR_X_POS+ICON_BATTERY_WIDTH+ICON_PLUG_WIDTH+2+2
#define ICON_VOLUME_WIDTH     (2+(2*SYSFONT_WIDTH))
#define ICON_PLAY_STATE_X_POS STATUSBAR_X_POS+ICON_BATTERY_WIDTH+ICON_PLUG_WIDTH+ICON_VOLUME_WIDTH+2+2+2
#define ICON_PLAY_STATE_WIDTH 7
#define ICON_PLAY_MODE_X_POS  STATUSBAR_X_POS+ICON_BATTERY_WIDTH+ICON_PLUG_WIDTH+ICON_VOLUME_WIDTH+ICON_PLAY_STATE_WIDTH+2+2+2+2
#define ICON_PLAY_MODE_WIDTH  7
#define ICON_SHUFFLE_X_POS    STATUSBAR_X_POS+ICON_BATTERY_WIDTH+ICON_PLUG_WIDTH+ICON_VOLUME_WIDTH+ICON_PLAY_STATE_WIDTH+ICON_PLAY_MODE_WIDTH+2+2+2+2+2
#define ICON_SHUFFLE_WIDTH    7
#define LOCK_X_POS            STATUSBAR_X_POS+ICON_BATTERY_WIDTH+ICON_PLUG_WIDTH+ICON_VOLUME_WIDTH+ICON_PLAY_STATE_WIDTH+ICON_PLAY_MODE_WIDTH+ICON_SHUFFLE_WIDTH+2+2+2+2+2+2
#define LOCK_WIDTH            5
#define ICON_DISK_WIDTH       12
#define ICON_DISK_X_POS       STATUSBAR_WIDTH-ICON_DISK_WIDTH
#define TIME_X_END            STATUSBAR_WIDTH-1

#if defined(SYSFONT_HEIGHT) && (SB_ICON_HEIGHT > STATUSBAR_HEIGHT)
#error "Icons larger than statusbar!"
#endif

#endif /*  _ICON_BITMAPS_H_ */
