/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to notes_viewer.c: the liner notes screen, a page at a time --
 * an album's, a person's, or any other -- and the links between them.
 ****************************************************************************/

#ifndef _NOTES_VIEWER_H_
#define _NOTES_VIEWER_H_

/* liner_notes_colours: where a page's colours come from */
enum
{
    NOTES_COLOURS_ART = 0,      /* its own picture, held to a deep or pale
                                   tint; a page without one keeps the last */
    NOTES_COLOURS_THEME,        /* the theme's, through dynamic colours */
    NOTES_COLOURS_TEXT_VIEWER,  /* the text viewer's colour mode */
    NOTES_COLOURS_CUSTOM,       /* text, links and page each chosen */
};

/* Under Custom, where the text, the links or the page take their colour */
enum
{
    NOTES_FROM_ART = 0,         /* the page's picture: its vivid colour for
                                   text and links, a deep or pale tint of
                                   its main one for the page */
    NOTES_FROM_THEME,
    NOTES_FROM_WHITE,
    NOTES_FROM_BLACK,
};

/* The page of the playing track's album, at the track. Each of these
 * returns a GO_TO_* code: GO_TO_DBBROWSER when the library browser is armed
 * with an album or an artist to go to, GO_TO_WPS when something was set
 * playing. */
int notes_viewer(void);

/* The page of the album `a`, whether or not it is playing */
struct notes_album;
int notes_album(const struct notes_album *a);

/* The page of the person called `name`: their written page, else one made
 * from the library and the pages that name them. notes_bio() is the same,
 * for the Biography rows. */
int notes_person(const char *name);
int notes_bio(const char *name);

#endif /* _NOTES_VIEWER_H_ */
