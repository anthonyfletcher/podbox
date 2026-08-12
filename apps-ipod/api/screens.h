/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Boundary stub for `uisimulator/common/stubs.c`, which includes "screens.h"
 * and uses nothing from it. Upstream's apps/screens.h declares the charging
 * splash, the MMC remove request and a handful of screens; this fork split
 * them across screens/, and stubs.c is an upstream file that cannot be edited
 * to follow.
 *
 * Like api/plugin.h, this stub forwards to nothing -- it **is** the empty
 * header. If stubs.c ever starts using a declaration from it, forward to
 * whichever apps-ipod/screens/ header now owns that symbol.
 *
 * Only a simulator build reaches this: nothing under firmware/ or lib/ that a
 * hardware build compiles includes "screens.h".
 ****************************************************************************/
#ifndef _SCREENS_H
#define _SCREENS_H

#endif
