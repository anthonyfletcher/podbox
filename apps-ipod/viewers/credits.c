/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The credits screen: the contributor list generated into credits.raw from
 * docs/CREDITS, crawling up the screen on the text reel.
 ****************************************************************************/
#include "config.h"
#include "text_reel.h"
#include "credits.h"

static const char* const credits[] = {
#include "credits.raw" /* generated list of names from docs/CREDITS */
};

#define CR_NUMNAMES  ((int)(sizeof(credits) / sizeof(credits[0])))

/* Display lines each name wraps to, the reel's to fill in. Names are short,
 * so a byte each is plenty. */
static unsigned char cr_lines[sizeof(credits) / sizeof(credits[0])];

int credits_screen(void)
{
    return text_reel_run(credits, cr_lines, CR_NUMNAMES);
}
