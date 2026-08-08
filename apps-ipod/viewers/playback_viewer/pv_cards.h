/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Interface to pv_cards.c.
 ****************************************************************************/
#ifndef _PV_CARDS_H
#define _PV_CARDS_H

#include "pv_paint.h"
#include "pv_paint.h"
#include "pv_stats.h"

/* The deck, in the order it is flipped through.
 *
 * The achievements card belongs between YEAR and HEAT and
 * is not built yet; adding it shifts every id after it, which matters
 * only to anything that persists a card id. Nothing does yet. */
enum pv_card
{
    PV_CARD_INTRO = 0,
    PV_CARD_MINUTES,
    PV_CARD_PLAYS,
    PV_CARD_ARTISTS,
    PV_CARD_SONGS,
    PV_CARD_ALBUMS,
    PV_CARD_CLOCK,
    PV_CARD_NIGHT,
    PV_CARD_SKIPS,
    PV_CARD_LOYAL,
    PV_CARD_REDIS,
    PV_CARD_YEAR,
    PV_CARD_ACH,
    PV_CARD_HEAT,
    PV_CARD_TYPE,
    PV_CARD_OUTRO,
    PV_CARD_COUNT
};

/* Draw one card and run whatever animation it has.
 *
 * 'dir' is the transition: 0 flushes the finished frame, +1/-1 wipes it in
 * from that direction.
 *
 * Returns 0 when the card finished drawing, or the button code that
 * interrupted it -- animations are abandoned the moment the user asks for
 * something, so a deck can be flipped through at speed rather than making
 * each card be sat through. The caller acts on that code as if it had come
 * from the button queue. */
int pv_cards_draw(int idx, int dir, const struct pv_totals *t);

/* A card's palette, for the one screen reached THROUGH a card that has to go
 * on looking like it: the week browser behind the year card. */
const struct pv_theme *pv_cards_theme(int idx);

/* A card's palette, for the one screen that is reached THROUGH a card and
 * has to keep looking like it -- the week browser behind the year card. */
const struct pv_theme *pv_cards_theme(int idx);

#endif /* _PV_CARDS_H */
