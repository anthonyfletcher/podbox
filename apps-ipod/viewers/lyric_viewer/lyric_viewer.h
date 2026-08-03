/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The synchronised-lyrics screen: three lines of the playing track, the one
 * being sung between the one before and the one after.
 ****************************************************************************/

#ifndef _LYRIC_VIEWER_H
#define _LYRIC_VIEWER_H

/* Show the lyrics of the playing track, returning a GO_TO_* code when the
 * user leaves. Needs something playing -- it synchronises to the track's
 * elapsed time -- and splashes rather than opening if there is nothing to
 * show. */
int lyric_viewer(void);

#endif /* _LYRIC_VIEWER_H */
