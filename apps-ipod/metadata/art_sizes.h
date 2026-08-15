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

/* Pixel order a size's thumbnails are stored in.
 *
 * Row-major is the default and what every general-purpose reader expects. A
 * size whose only consumer wants columns can ask for them instead, and pay the
 * transpose once here at generation rather than on every load: the carousel
 * evicts and reloads slides constantly as it scrolls, so a per-load transpose
 * is the same work done over and over.
 *
 * Only square thumbnails are stored transposed -- that is an in-place swap and
 * needs no second buffer. AA_FIT_COVER is square except when the source is too
 * elongated to crop and falls back to CONTAIN, so the writer records the layout
 * it actually used in the file header and readers honour that, not this. */
enum art_layout
{
    AA_ROWS = 0,   /* row-major: pixel (x,y) at y*width + x */
    AA_COLUMNS     /* column-major: pixel (x,y) at x*height + y */
};

struct art_size
{
    const char *name;       /* cache sub-folder name / stable identifier   */
    short       dim;        /* target square edge in pixels (NxN)          */
    enum albumart_fit fit;  /* how to fit the source into the square       */
    enum art_layout layout; /* pixel order to store it in                  */
};

/* Thumbnail resolutions the cache generates. The order here does not matter:
 * generation sorts by dim itself, because the "fast build" setting derives each
 * size from the next larger one. Keep ART_CACHE_MAX_DIM >= the largest 'dim';
 * it sizes the decode work buffer. Changing a dim or fit alters the cached
 * pixels, so bump ART_CACHE_FORMAT_VERSION to force a regenerate. */
static const struct art_size art_sizes[] =
{
    /* Columns because the carousel renders slides column by column for the
     * perspective transform, and it is this size's only reader. */
    { "coverflow", 128, AA_FIT_COVER, AA_COLUMNS },/* carousel slides (DISPLAY_WIDTH) */
    { "list",       44, AA_FIT_COVER, AA_ROWS    },/* database album rows, in a 46px row */
    { "wps",       300, AA_FIT_COVER, AA_ROWS    },/* now-playing / miniplayer art */
};

#define ART_CACHE_NUM_SIZES \
    ((int)(sizeof(art_sizes) / sizeof(art_sizes[0])))

/* Must be >= the largest 'dim' in art_sizes[] above. */
#define ART_CACHE_MAX_DIM 300

/* Sizes the decode work buffer (see aa_run_pass()), which stages the largest
 * size at this aspect before cropping it square. Raising it costs memory during
 * cache generation. */
#define ART_CACHE_COVER_MAX_ASPECT 2

/* What AA_FIT_COVER tests against. The work buffer is not a plain rectangle, so
 * two things bound a staged image and neither implies the other:
 *
 *   Area, because BM_SCALED_SIZE's pixel term is width * height. Budgeting by
 *   area rather than by ratio is what lets a small thumbnail be cropped from a
 *   more elongated source than a large one, at no extra cost -- the 300px size
 *   still stops at 2:1 while the 128px carousel size reaches nearly 5:1, past
 *   any real album art. Worth having, because a letterboxed thumbnail is stored
 *   by rows rather than transposed, and then costs a transpose on every load.
 *
 *   Width, because BM_SCALED_SIZE's *other* term -- the scaler's line buffers,
 *   which resize_on_load() sizes at 3 rows of uint32_argb -- grows with the
 *   width alone, and area says nothing about it. Without this bound a 1400x128
 *   stage passes the pixel budget while wanting two and a half times the
 *   scratch the buffer was cut for. resize_on_load() would refuse rather than
 *   overrun, so nothing breaks -- but aa_generate_one() has committed to a
 *   COVER decode by then and does not fall back to CONTAIN, so the thumbnail
 *   would simply never appear. Refusing here instead is what routes it to the
 *   letterboxed decode that does work.
 *
 * Height needs no bound of its own: a tall stage is narrow, so it spends
 * nothing on scratch and the area test alone holds it. */
#define ART_CACHE_COVER_MAX_PX \
    ((long)ART_CACHE_MAX_DIM * ART_CACHE_COVER_MAX_ASPECT * ART_CACHE_MAX_DIM)

#define ART_CACHE_COVER_MAX_W \
    (ART_CACHE_MAX_DIM * ART_CACHE_COVER_MAX_ASPECT)

#endif /* _ART_SIZES_H_ */
