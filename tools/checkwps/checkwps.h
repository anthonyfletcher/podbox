/***************************************************************************
 *             __________               __   ___.                  
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___  
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /  
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <   
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \  
 *                     \/            \/     \/    \/            \/ 
 *
 * Copyright (C) 2008 by Jonathan Gordon
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef _CHECKWPS_H_
#define _CHECKWPS_H_
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

struct viewport;

/* The UI viewport of the last .sbs this run loaded. stubs.c returns it from
 * sb_skin_get_info_vp(), so a .wps named after an .sbs on the command line
 * inherits its colours the way it does on the player. */
extern struct viewport checkwps_sbs_info_vp;
extern bool checkwps_have_sbs_info_vp;

#endif
