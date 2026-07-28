/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The album-art sizes this build caches, derived from the skins in use.
 ****************************************************************************/

#ifndef _ART_SIZES_H_
#define _ART_SIZES_H_

/* How a source image is fitted into the square NxN thumbnail.
 *
 * AA_FIT_CONTAIN : scale to fit inside NxN preserving aspect (letterbox). The
 *                  cached image is therefore NOT square for non-square sources;
 *                  its real dimensions are in the cache header.
 * AA_FIT_COVER   : scale so the shorter side == N, then centre-crop the longer
 *                  side to N. Always yields exactly NxN, cropping non-square art.
 *
 * COVER needs a non-square intermediate (N x N*aspect) before the crop, so it is
 * only applied while the source's aspect ratio is within
 * ART_CACHE_COVER_MAX_ASPECT; a more elongated source falls back to CONTAIN
 * rather than blow up the decode work buffer. Album art beyond 2:1 is vanishingly
 * rare, so raise the cap only if you actually hit it (it costs work-buffer bytes).
 */
enum albumart_fit
{
    AA_FIT_CONTAIN = 0,
    AA_FIT_COVER,
};

struct art_size
{
    const char *name;       /* cache sub-folder name / stable identifier   */
    short       dim;        /* target square edge in pixels (NxN)          */
    enum albumart_fit fit;  /* how to fit the source into the square       */
};

/* Thumbnail resolutions the cache generates. The order here does not matter:
 * generation sorts by dim itself, because the "fast build" setting derives each
 * size from the next larger one. Keep ART_CACHE_MAX_DIM >= the largest 'dim';
 * it sizes the decode work buffer. Changing a dim or fit alters the cached
 * pixels, so bump ART_CACHE_FORMAT_VERSION to force a regenerate. */
static const struct art_size art_sizes[] =
{
    { "coverflow", 128, AA_FIT_COVER   },   /* carousel slides (DISPLAY_WIDTH) */
    { "list",       44, AA_FIT_COVER   },   /* database album rows, in a 46px row */
    { "wps",       300, AA_FIT_COVER   },   /* now-playing / miniplayer art */
};

#define ART_CACHE_NUM_SIZES \
    ((int)(sizeof(art_sizes) / sizeof(art_sizes[0])))

/* Must be >= the largest 'dim' in art_sizes[] above. */
#define ART_CACHE_MAX_DIM 300

/* Widest source aspect ratio AA_FIT_COVER will crop; beyond this the size falls
 * back to CONTAIN. Sizes the decode work buffer (see aa_run_pass()), so raising
 * it costs memory during cache generation. */
#define ART_CACHE_COVER_MAX_ASPECT 2

#endif /* _ART_SIZES_H_ */
