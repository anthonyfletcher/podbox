/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to album_charts.c.
 ****************************************************************************/
#ifndef _ALBUM_CHARTS_H
#define _ALBUM_CHARTS_H

/* Which ranking, over albums or over artists. Appended only -- load_root()
 * stores these in a tagentry's extraseek and browser_db_enter() reads them
 * back, so renumbering changes what an already-drawn row means. */
enum album_chart {
    ALBUM_CHART_MOST_PLAYED = 0,
    ALBUM_CHART_RECENTLY_PLAYED,
    ALBUM_CHART_FORGOTTEN,
    ARTIST_CHART_MOST_PLAYED,
    ARTIST_CHART_RECENTLY_PLAYED,
    ARTIST_CHART_FORGOTTEN,
};

/* Do the artists rather than the albums? The two halves rank identically and
 * differ only in which array they read, so one set of comparisons serves
 * both. */
#define CHART_IS_ARTIST(k) ((k) >= ARTIST_CHART_MOST_PLAYED)

/* Show one chart. Owns the screen until the user leaves. Returns a GO_TO_*
 * code -- the database browser's when an album was opened, GO_TO_PREVIOUS
 * when backed out of or when there is nothing to rank. */
int album_charts_show(enum album_chart kind);

/* Which chart the next album_charts_run() shows. The database browser's rows
 * arm this before returning GO_TO_ALBUM_CHARTS, because a browse level can
 * only hand back a bare screen code and root_menu.c's dispatch table has
 * nowhere to carry a parameter. */
void album_charts_arm(enum album_chart kind);
int album_charts_run(void);

/* Pick an album at random and start playing it. GO_TO_WPS on success,
 * GO_TO_PREVIOUS if it could not. Needs no playback history, so this works
 * with runtime data gathering off. */
int album_random(void);

#endif /* _ALBUM_CHARTS_H */
