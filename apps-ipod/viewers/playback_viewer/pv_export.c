/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/wrapped_core.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * Saving cards as pictures.
 *
 * screen_dump() is the firmware's own screenshot: it writes the framebuffer
 * to a BMP in the device root and names the file after the clock. It returns
 * nothing, so there is no way to ask which file it just wrote. A card that
 * wants a name of its own therefore has to be dumped, found again as the
 * newest dump in the root, and renamed.
 *
 * Nothing else in this fork calls screen_dump(), so this is also the only
 * place that knows any of the above.
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <file.h>
#include <dir.h>
#include "config.h"
#include "kernel.h"
#include "system.h"          /* cpu_boost */
#include "button.h"
#include "screendump.h"
#include "widgets/splash.h"
#include "pv_paint.h"
#include "pv_cards.h"
#include "pv_export.h"

/* Where a whole-deck export goes. Named for what the user calls the feature,
 * because they are the one who will find the folder. */
#define PV_EXPORT_DIR "/spun_cards"

#ifdef HAVE_SCREENDUMP

/* Rename the newest "dump*.bmp" in the root to 'dst'. False if there was
 * none -- which means the dump itself failed, since one was just taken. */
static bool claim_newest_dump(const char *dst)
{
    char newest[MAX_PATH];
    unsigned long best = 0;
    struct dirent *e;
    DIR *d;

    d = opendir("/");
    if (!d)
        return false;

    newest[0] = '\0';
    while ((e = readdir(d)) != NULL)
    {
        int len = strlen(e->d_name);
        struct dirinfo info;

        if (len < 9 || strncmp(e->d_name, "dump", 4) != 0
            || strcasecmp(e->d_name + len - 4, ".bmp") != 0)
            continue;

        /* Newest wins. The one just written has the latest timestamp, and
         * older dumps the user made themselves are left alone. */
        info = dir_get_info(d, e);
        if ((unsigned long)info.mtime >= best)
        {
            best = (unsigned long)info.mtime;
            snprintf(newest, sizeof(newest), "/%s", e->d_name);
        }
    }
    closedir(d);

    if (!newest[0])
        return false;

    remove(dst);            /* re-exporting replaces */
    return rename(newest, dst) == 0;
}

void pv_export_card(void)
{
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(true);        /* converting a screenful to a BMP is not free */
#endif
    screen_dump();
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(false);
#endif
    splash(HZ, "Saved to the device root");
}

void pv_export_deck(const struct pv_totals *t)
{
    int saved = 0;

    mkdir(PV_EXPORT_DIR);

    /* Every card draws its final frame straight away from here on: a dump
     * taken mid-animation would picture a number still counting up. */
    pv_set_export(true);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    /* A full card is gradient and per-pixel blending edge to edge, and there
     * are sixteen of them to render and convert. */
    cpu_boost(true);
#endif

    for (int i = 0; i < PV_CARD_COUNT; i++)
    {
        char dst[40];

        pv_cards_draw(i, 0, t);

        screen_dump();
        snprintf(dst, sizeof(dst), PV_EXPORT_DIR "/card_%02d.bmp", i + 1);
        if (claim_newest_dump(dst))
            saved++;

        /* Anything pressed during the export would otherwise be waiting to
         * navigate the moment it finished. */
        button_clear_queue();
    }

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(false);
#endif
    pv_set_export(false);

    splashf(HZ * 2, "%d cards saved in %s", saved, PV_EXPORT_DIR);
}

#else /* !HAVE_SCREENDUMP */

void pv_export_card(void)
{
    splash(HZ, "No screendump in this build");
}

void pv_export_deck(const struct pv_totals *t)
{
    (void)t;
    splash(HZ, "No screendump in this build");
}

#endif /* HAVE_SCREENDUMP */
