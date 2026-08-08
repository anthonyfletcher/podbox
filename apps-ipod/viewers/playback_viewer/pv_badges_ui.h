/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/achievements_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_badges_ui.c.
 ****************************************************************************/
#ifndef _PV_BADGES_UI_H
#define _PV_BADGES_UI_H

#include "pv_paint.h"
#include "pv_stats.h"

/* The achievements card: how much of the wall is lit, and an invitation to
 * go and look. Drawn by pv_cards like any other card. */
int pv_badges_card(const struct pv_theme *th, int idx, int dir,
                   const struct pv_totals *t);

/* The wall itself, entered with SELECT from that card: one scrolling row per
 * badge, with the selected row's description and progress underneath.
 *
 * Returns SYS_USB_CONNECTED if that arrived while browsing -- the caller must
 * pass it on -- or 0 on a normal exit. */
int pv_badges_browse(const struct pv_totals *t);

#endif /* _PV_BADGES_UI_H */
