/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_year.c.
 ****************************************************************************/
#ifndef _PV_YEAR_H
#define _PV_YEAR_H

#include <stdbool.h>
#include "pv_paint.h"
#include "pv_stats.h"

/* The year card: how far through the year you are, and a bar of its weeks.
 * Drawn by pv_cards like any other card; it lives here because the week
 * machinery behind it is a good deal more than a card's worth. */
int pv_year_card(const struct pv_theme *th, int idx, int dir,
                 const struct pv_totals *t);

/* The week browser, entered with SELECT from the year card: the wheel moves
 * a cursor along the bar, SELECT again opens that week's recap, MENU backs
 * out one level at a time.
 *
 * Returns SYS_USB_CONNECTED if that arrived while browsing -- the caller must
 * pass it on -- or 0 on a normal exit. */
int pv_year_browse(const struct pv_theme *th, const struct pv_totals *t);

/* False when there is no year to browse: an all-time view on a device whose
 * clock was never set has no calendar to hang weeks on. The card says so
 * itself; this is for the caller deciding whether SELECT should do anything. */
bool pv_year_available(void);

#endif /* _PV_YEAR_H */
