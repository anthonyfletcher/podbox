/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The About page: docs/podbox/ABOUT, compiled into the firmware and crawled
 * up the screen on the text reel.
 ****************************************************************************/

#include "config.h"
#include "viewers/text_reel.h"
#include "about.h"

/* docs/podbox/ABOUT, escaped into one string literal per line by the rule in
 * apps.make. In the binary rather than shipped as a file, so the page is there
 * whatever the disk holds -- the document is a few hundred bytes. Its blank
 * lines separate the paragraphs on the reel, so they are entries like any
 * other. */
static const char* const about_text[] = {
#include "about.raw"
};

#define AB_NUMLINES  ((int)(sizeof(about_text) / sizeof(about_text[0])))

/* Display lines each line of the document wraps to, the reel's to fill in.
 * A paragraph is a single entry here, so this counts whole paragraphs -- still
 * well inside a byte at the screen's width. */
static unsigned char ab_lines[sizeof(about_text) / sizeof(about_text[0])];

int about_screen(void)
{
    return text_reel_run(about_text, ab_lines, AB_NUMLINES);
}
