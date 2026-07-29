/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The album index: the flat album and artist list the carousel scrolls.
 *
 * Built by walking tagcache, held in the buffer carousel.h describes as
 * pf_idx, and persisted to carousel.idx so later opens read it back instead
 * of rescanning. It carries no artwork -- only names, years and the taglist
 * seeks needed to navigate into the database -- so it goes stale when the
 * database changes, not when files on disk do.
 *
 * It lives here rather than in album_covers.c because building it is not the
 * carousel's job: both carousel models consume it, and the build is intended
 * to run while neither is on screen.
 *
 * Parts, in order:
 *   - progress reporting and cancellation
 *   - sort comparators
 *   - writing entries into the index buffer
 *   - the tagcache walks that populate it
 *   - the on-disk form
 *   - album_index_build(), which reuses the saved index or rebuilds it
 ****************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"          /* ALIGN_BUFFER/alignof */
#include "rbpaths.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"                     /* dir_exists, mkdir */
#include "lang.h"
#include "settings/settings.h"
#include "database/tagcache.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"           /* gui_syncyesno_run, YESNO_YES */
#include "draw/screen_access.h"
#include "skin/statusbar_skinned.h"  /* sb_skin_update */
#include "skin/skin_engine.h"        /* skin_flush_dirty */
#include "input/action.h"
#include "powermgmt.h"               /* reset_poweroff_timer */
#include "system/shutdown.h"
#include "system/activity.h"          /* ui_set_working */
#include "core_alloc.h"              /* background build buffer */
#include "usb.h"                     /* SYS_USB_CONNECTED handling */
#include "screens/covers/carousel.h"      /* pf_idx, CACHE_PREFIX, SUCCESS/ERROR_* */
#include "screens/covers/album_covers.h"   /* SORT_BY_*, ASCENDING (sort order) */
#include "album_index.h"

/* The album index file, and the four-byte magic at its start. Holds the
 * artist name blob as well as the albums -- artist_portraits.c reads
 * pf_idx.artist_names/artist_index straight out of it -- so the name is
 * deliberately not album-specific. Regenerated if absent. */
#define ALBUM_INDEX CACHE_PREFIX "/carousel.idx"
#define INDEX_HDR "PFID"

enum ePFS { ePFS_ARTIST = 0, ePFS_ALBUM };

/* Shared by every walk below; the models keep their own where they need one. */
static struct tagcache_search tcs;

/* The index currently being built, and the buffer it is being built into.
 *
 * A file static rather than a parameter threaded through the ten functions
 * that touch it, and deliberately singular: only one build may run at a time,
 * whoever asked for it. That lets the same code serve the carousel building
 * into the app buffer it has claimed, and a background build into memory of
 * its own -- the allocation differs, the builder does not. */
static struct pf_index_t *pfi;

/* Serialises the two callers of the builder. The carousel blocks here rather
 * than starting a second build; because it has usually just asked for one, it
 * sets idx_abort first so the background pass gives up promptly instead of
 * making the screen wait out a full scan. */
static struct mutex build_mutex;

/* True while the *background* pass owns the builder. It changes two things
 * the builder must not do off the main thread: draw, and read the keypad. */
static bool building_bg;
static volatile bool idx_abort;

/* Last progress the background pass reported; read by anything showing it. */
static int bg_done, bg_total;

static void draw_progressbar(int step, int count, char *msg);

static void draw_progressbar(int step, int count, char *msg)
{
    if (building_bg)
    {
        /* Record it instead. Nothing may reach the LCD from the background
         * thread -- whatever screen the user is on owns it. */
        bg_done = step;
        bg_total = count;
        return;
    }

    (void)step; (void)count; (void)msg;

    /* Rate-limited, because this is not cheap: a status bar render plus an LCD
     * flush is a few milliseconds either side of ten, and the callers reach
     * here once per album. Unthrottled, most of a foreground build was spent
     * redrawing rather than reading the database -- the background pass, which
     * draws nothing, finishes the same work several times quicker. 8fps is
     * more than enough for an indicator that only spins. */
    static long next_draw_tick;
    long now = current_tick;

    if (!TIME_AFTER(now, next_draw_tick))
        return;
    next_draw_tick = now + HZ / 8;

    sb_skin_update(SCREEN_MAIN, true);
    skin_flush_dirty();
}

static bool progress_cancel(int step, int count, char *msg)
{
    if (building_bg)
    {
        /* No get_action() here: the background pass must not consume button
         * presses meant for the screen in front of the user. It gives up when
         * someone else wants the builder, or on the way to a USB session.
         *
         * The yield is not optional. Scheduling is cooperative, so a pass that
         * never gives up the CPU freezes the UI however low its priority --
         * and get_action(), which used to be the yield point on this path, is
         * exactly what was removed above. */
        yield();
        if (count)
            draw_progressbar(step, count, msg);
        return idx_abort;
    }

    const struct text_message prompt = {
            (const char*[]) {"Quit?", "Progress will be lost"}, 2};

    int action = get_action(CONTEXT_STD,TIMEOUT_NOBLOCK);
    if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
    {
        if (gui_syncyesno_run(&prompt, NULL, NULL) == YESNO_YES)
            return true;
        lcd_clear_display();
    }

    if (count)
        draw_progressbar(step, count, msg);

    return false;
}

int compare_albums (const void *a_v, const void *b_v)
{
    uint32_t artist_a = ((struct album_data *)a_v)->artist_idx;
    uint32_t artist_b = ((struct album_data *)b_v)->artist_idx;

    uint32_t album_a = ((struct album_data *)a_v)->name_idx;
    uint32_t album_b = ((struct album_data *)b_v)->name_idx;

    int year_a = ((struct album_data *)a_v)->year;
    int year_b = ((struct album_data *)b_v)->year;

    switch (global_settings.album_covers_sort_albums_by)
    {
        case SORT_BY_ARTIST_AND_NAME:
            if (artist_a - artist_b == 0)
                return (int)(album_a - album_b);
            break;
        case SORT_BY_ARTIST_AND_YEAR:
            if (artist_a - artist_b == 0)
            {
                if (global_settings.album_covers_year_sort_order == ASCENDING)
                    return year_a - year_b;
                else
                    return year_b - year_a;
            }
            break;
        case SORT_BY_YEAR:
            if (year_a - year_b != 0)
            {
                if (global_settings.album_covers_year_sort_order == ASCENDING)
                    return year_a - year_b;
                else
                    return year_b - year_a;
            }
            break;
        case SORT_BY_NAME:
            if (album_a - album_b != 0)
                return (int)(album_a - album_b);
            break;
    }

    return (int)(artist_a - artist_b);
}

static int compare_album_artists (const void *a_v, const void *b_v)
{
    uint32_t a = ((struct album_data *)a_v)->artist_idx;
    uint32_t b = ((struct album_data *)b_v)->artist_idx;
    return (int)(a - b);
}

static void write_album_index(int idx, int name_idx,
                              long album_seek, int artist_idx, long artist_seek)
{
    pfi->album_index[idx].name_idx = name_idx;
    pfi->album_index[idx].seek = album_seek;
    pfi->album_index[idx].artist_idx = artist_idx;
    pfi->album_index[idx].artist_seek = artist_seek;
    pfi->album_index[idx].year = 0;
}

static inline void write_album_entry(struct tagcache_search *tcs,
                                     int name_idx, unsigned int len)
{
    write_album_index(-pfi->album_ct, name_idx, tcs->result_seek, 0, -1);
    pfi->album_len += len;
    pfi->album_ct++;

    if (pfi->album_untagged_seek == -1 && strcmp(UNTAGGED, tcs->result) == 0)
    {
        pfi->album_untagged_idx = name_idx;
        pfi->album_untagged_seek = tcs->result_seek;
    }
}

static void write_artist_entry(struct tagcache_search *tcs,
                               int name_idx, unsigned int len)
{
    pfi->artist_index[-pfi->artist_ct].name_idx = name_idx;
    pfi->artist_index[-pfi->artist_ct].seek = tcs->result_seek;
    pfi->artist_len += len;
    pfi->artist_ct++;
}

/* adds tagcache_search results into artist/album index */
static int get_tcs_search_res(int type, struct tagcache_search *tcs,
                              void **buf, size_t *bufsz)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int ret = SUCCESS;
    unsigned int l, name_idx = 0;
    void (*writefn)(struct tagcache_search *, int, unsigned int);
    int data_size;
    if (type == ePFS_ARTIST)
    {
        writefn = &write_artist_entry;
        data_size = sizeof(struct artist_data);
    }
    else
    {
        writefn = &write_album_entry;
        data_size = sizeof(struct album_data);
    }

    while (tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
    {
        if (progress_cancel(0, 0, NULL))
        {
            ret = ERROR_USER_ABORT;
            break;
        }

        l = tcs->result_len;

        /* Check before subtracting -- *bufsz is unsigned, so subtracting
         * data_size (or l) once the real remaining space is smaller than
         * that would wrap to a huge value instead of going negative,
         * permanently defeating this check for the rest of the scan and
         * letting strcpy()/writefn() below walk past the end of
         * pfi->buf on any library large enough to fill it. */
        if ((size_t)data_size > *bufsz || l > *bufsz - data_size)
        {
            /* not enough memory */
            ret = ERROR_BUFFER_FULL;
            break;
        }

        *bufsz -= data_size;

        strcpy(*buf, tcs->result);

        *bufsz -= l;
        *buf = l + (char *)*buf;

        writefn(tcs, name_idx, l);

        name_idx += l;
    }
    tagcache_search_finish(tcs);
    return ret;
}

#define STR_STEP_INDEXING_UNTAGGED "1/5 Find " UNTAGGED
#define STR_STEP_ASSIGNING_ALBUMS "2/5 Find Albums"
#define STR_STEP_ASSIGNING_ALBUM_YEAR "3/5 Check Album Year"
#define STR_STEP_REMOVING_DUPLICATES "4/5 Remove Duplicates"

/*adds <untagged> albums/artist to existing album index */
static int create_album_untagged(struct tagcache_search *tcs, size_t *bufsz)
{
    static char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int ret = SUCCESS;
    int album_count = pfi->album_ct; /* store existing count */
    int total_count = pfi->album_ct + pfi->artist_ct * 2;
    long seek;
    int last, final, retry;
    int i, j;
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, total_count, STR_STEP_INDEXING_UNTAGGED);

    /* search tagcache for all <untagged> albums & save the albumartist seek pos */
    if (tagcache_search(tcs, tag_albumartist))
    {
        tagcache_search_add_filter(tcs, tag_album, pfi->album_untagged_seek);

        while (tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
        {
            if (progress_cancel(pfi->album_ct, total_count, STR_STEP_INDEXING_UNTAGGED))
            {
                tagcache_search_finish(tcs);
                return ERROR_USER_ABORT;
            }

            if (tcs->result_seek ==
                pfi->album_index[-(pfi->album_ct - 1)].artist_seek)
                continue;

            if (sizeof(struct album_data) > *bufsz)
            {
                /* not enough memory */
                ret = ERROR_BUFFER_FULL;
                break;
            }

            *bufsz -= sizeof(struct album_data);
            write_album_index(-pfi->album_ct, pfi->album_untagged_idx,
                               pfi->album_untagged_seek, -1, tcs->result_seek);

            pfi->album_ct++;
        }
        tagcache_search_finish(tcs);

        if (ret == SUCCESS) {
            draw_progressbar(0, pfi->album_ct, STR_STEP_INDEXING_UNTAGGED);

            last = 0;
            final = pfi->artist_ct;
            retry = 0;

            /* map the artist_seek position to the artist name index */
            for (j = album_count; j < pfi->album_ct; j++)
            {
                if (progress_cancel(j, pfi->album_ct, STR_STEP_INDEXING_UNTAGGED))
                    return ERROR_USER_ABORT;

                seek = pfi->album_index[-j].artist_seek;

    retry_artist_lookup:
                retry++;
                for (i = last; i < final; i++)
                {
                    if (seek == pfi->artist_index[i].seek)
                    {
                        int idx = pfi->artist_index[i].name_idx;
                        pfi->album_index[-j].artist_idx = idx;
                        last = i; /* last match, start here next loop */
                        final = pfi->artist_ct;
                        retry = 0;
                        break;
                    }
                }
                if (retry > 0 && retry < 2)
                {
                    /* no match start back at beginning */
                    final = last;
                    last = 0;
                    goto retry_artist_lookup;
                }
            }
        }
    }

    return ret;
}

/* Create an index of all artists from the database */
int build_artist_index(struct tagcache_search *tcs,
                                 void **buf, size_t *bufsz)
{
    int i, res = SUCCESS;
    struct artist_data* tmp_artist;

    /* artist index starts at end of buf it will be rearranged when finalized */
    pfi->artist_index = ((struct artist_data *)(*bufsz + (char *) *buf)) - 1;
    pfi->artist_ct = 0;
    pfi->artist_len = 0;
    /* artist names starts at beginning of buf */
    pfi->artist_names = *buf;

    tagcache_search(tcs, tag_albumartist);
    res = get_tcs_search_res(ePFS_ARTIST, tcs, &(*buf), bufsz);
    tagcache_search_finish(tcs);
    if (res < SUCCESS)
        return res;

    /* finalize the artist index */
    ALIGN_BUFFER(*buf, *bufsz, alignof(struct artist_data));
    tmp_artist = (struct artist_data*)*buf;
    for (i = pfi->artist_ct - 1; i >= 0; i--)
        tmp_artist[i] = pfi->artist_index[-i];

    pfi->artist_index = tmp_artist;
    /* move buf ptr to end of artist_index */
    *buf = pfi->artist_index + pfi->artist_ct;

    if (res == SUCCESS)
    {
        if (pfi->artist_ct > 0)
            res = pfi->artist_ct;
        else
            res = ERROR_NO_ALBUMS;
    }

    return res;
}

static int assign_album_year(void)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pfi->album_ct, STR_STEP_ASSIGNING_ALBUM_YEAR);
    for (int album_idx = 0; album_idx < pfi->album_ct; album_idx++)
    {
        /* Prevent idle poweroff */
        reset_poweroff_timer();

        if (progress_cancel(album_idx, pfi->album_ct, STR_STEP_ASSIGNING_ALBUM_YEAR))
            return ERROR_USER_ABORT;

        int album_year = 0;

        if (tagcache_search(&tcs, tag_year))
        {
            tagcache_search_add_filter(&tcs, tag_album,
                                       pfi->album_index[album_idx].seek);

            if (pfi->album_index[album_idx].artist_idx >= 0)
                tagcache_search_add_filter(&tcs, tag_albumartist,
                    pfi->album_index[album_idx].artist_seek);

            while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz)) {
                int track_year = tagcache_get_numeric(&tcs, tag_year);
                if (track_year > album_year)
                    album_year = track_year;
            }
        }
        tagcache_search_finish(&tcs);

        pfi->album_index[album_idx].year = album_year;
    }
    return SUCCESS;
}

/**
  Create an index of all artists and albums from the database.
  Also store the artists and album names so we can access them later.
 */
static int create_album_index(void)
{
    static char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    void *buf = pfi->buf;
    size_t buf_size = pfi->buf_sz;

    struct album_data* tmp_album;

    int i, j, last, final, retry, res;

    ALIGN_BUFFER(buf, buf_size, sizeof(long));

    /* Artists */
    res = build_artist_index(&tcs, &buf, &buf_size);
    if (res < SUCCESS)
        return res;

    /* Albums */
    pfi->album_ct = 0;
    pfi->album_len =0;
    pfi->album_untagged_idx = -1;
    pfi->album_untagged_seek = -1;

    /* album_index starts at end of buf it will be rearranged when finalized */
    pfi->album_index = ((struct album_data *)(buf_size + (char *)buf)) - 1;
    /* album_names starts at the beginning of buf */
    pfi->album_names = buf;

    tagcache_search(&tcs, tag_album);
    res = get_tcs_search_res(ePFS_ALBUM, &tcs, &buf, &buf_size);
    tagcache_search_finish(&tcs);
    if (res < SUCCESS)
        return res;

    /* Build artist list for untagged albums */
    res = create_album_untagged(&tcs, &buf_size);

    if (res < SUCCESS)
        return res;

    /* finalize the album index */
    ALIGN_BUFFER(buf, buf_size, alignof(struct album_data));
    tmp_album = (struct album_data*)buf;
    for (i = pfi->album_ct - 1; i >= 0; i--)
        tmp_album[i] = pfi->album_index[-i];

    pfi->album_index = tmp_album;
    /* move buf ptr to end of album_index */
    buf = pfi->album_index + pfi->album_ct;

    /* Assign indices */
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pfi->album_ct, STR_STEP_ASSIGNING_ALBUMS);
    for (j = 0; j < pfi->album_ct; j++)
    {
        /* Prevent idle poweroff */
        reset_poweroff_timer();

        if (progress_cancel(j, pfi->album_ct, STR_STEP_ASSIGNING_ALBUMS))
            return ERROR_USER_ABORT;

        if (pfi->album_index[j].artist_seek >= 0) { continue; }

        tagcache_search(&tcs, tag_albumartist);
        tagcache_search_add_filter(&tcs, tag_album, pfi->album_index[j].seek);

        last = 0;
        final = pfi->artist_ct;
        retry = 0;
        if (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
        {

retry_artist_lookup:
            retry++;
            for (i = last; i < final; i++)
            {
                if (tcs.result_seek == pfi->artist_index[i].seek)
                {
                    int idx = pfi->artist_index[i].name_idx;
                    pfi->album_index[j].artist_idx = idx;
                    pfi->album_index[j].artist_seek = tcs.result_seek;
                    last = i; /* last match, start here next loop */
                    final = pfi->artist_ct;
                    retry = 0;
                    break;
                }
            }
            if (retry > 0 && retry < 2)
            {
                /* no match start back at beginning */
                final = last;
                last = 0;
                goto retry_artist_lookup;
            }
        }
        tagcache_search_finish(&tcs);
    }

    res = assign_album_year();

    if (res < SUCCESS)
        return res;

    /* sort list order to find duplicates */
    qsort(pfi->album_index, pfi->album_ct,
              sizeof(struct album_data), compare_album_artists);

    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pfi->album_ct, STR_STEP_REMOVING_DUPLICATES);
    /* mark duplicate albums for deletion */
    for (i = 0; i < pfi->album_ct - 1; i++) /* -1 don't check last entry */
    {
        /* Prevent idle poweroff */
        reset_poweroff_timer();

        if (progress_cancel(i, pfi->album_ct, STR_STEP_REMOVING_DUPLICATES))
            return ERROR_USER_ABORT;

        int idxi = pfi->album_index[i].artist_idx;
        int seeki = pfi->album_index[i].seek;

        for (j = i + 1; j < pfi->album_ct; j++)
        {
            if (idxi > 0 &&
            idxi == pfi->album_index[j].artist_idx &&
            seeki == pfi->album_index[j].seek)
            {
                pfi->album_index[j].artist_idx = -1;
            }
            else
            {
                i = j - 1;
                break;
            }
        }
    }

    /* now fix the album list order */
    qsort(pfi->album_index, pfi->album_ct,
              sizeof(struct album_data), compare_album_artists);

    /* remove any extra untagged albums
     * extra space is orphaned till restart */
    pfi->album_index += pfi->album_untagged_idx + 1;
    pfi->album_ct -= pfi->album_untagged_idx + 1;

    pfi->buf = buf;
    pfi->buf_sz = buf_size;
    pfi->artist_index = 0;

    qsort(pfi->album_index, pfi->album_ct,
                          sizeof(struct album_data), compare_albums);

    return (pfi->album_ct > 0) ? 0 : ERROR_NO_ALBUMS;
}

/*Saves the album index into a binary file to be recovered the
 next time PictureFlow is launched*/

static int save_album_index(void){
    int fd = creat(ALBUM_INDEX,0666);

    struct pf_index_t data;
    memcpy(&data, pfi, sizeof(struct pf_index_t));

    if(fd >= 0)
    {
        memcpy(&data.header, INDEX_HDR, sizeof(pfi->header));

        write(fd, &data, sizeof(struct pf_index_t));

        write(fd, data.artist_names, data.artist_len);
        write(fd, data.album_names, data.album_len);

        write(fd, data.album_index, data.album_ct * sizeof(struct album_data));

        close(fd);
        return 0;
    }
    return -1;
}

/* reads data from save file to buffer */
static inline int read2buf(int fildes, void *buf, size_t nbyte){
    int nread;
    nread = read(fildes, buf, nbyte);
    if (nread < (int)nbyte)
        return 0;

    return nread;
}

/*Loads the album_index information stored in the hard drive*/
static int load_album_index(void){

    int i, fr = open(ALBUM_INDEX, O_RDONLY);
    struct pf_index_t data;

    void *bufstart = pfi->buf;
    unsigned int bufstart_sz = pfi->buf_sz;

    void* buf = pfi->buf;
    size_t buf_size = pfi->buf_sz;

    unsigned int name_sz, album_idx_sz;
    int album_idx, artist_idx;

    if (fr >= 0){
        const unsigned long fsize = filesize(fr);
        if (fsize > sizeof(data))
        {
            if (read(fr, &data, sizeof(data)) == sizeof(data) &&
                memcmp(&(data.header), INDEX_HDR, sizeof(data.header)) == 0)
            {
                name_sz = data.artist_len + data.album_len;
                album_idx_sz = data.album_ct * sizeof(struct album_data);

                if (name_sz + album_idx_sz > bufstart_sz)
                    goto failure;

                /* lseek(fr, sizeof(data) + 1, SEEK_SET); */
                /* artist names */
                if (read2buf(fr, buf, data.artist_len) == 0)
                    goto failure;

                data.artist_names = buf;
                buf = (char *)buf + data.artist_len;
                buf_size -= data.artist_len;

                /* album names */
                if (read2buf(fr, buf, data.album_len) == 0)
                    goto failure;

                data.album_names = buf;
                buf = (char *)buf + data.album_len;
                buf_size -= data.album_len;

                /* index of album names */
                ALIGN_BUFFER(buf, buf_size, alignof(struct album_data));
                if (read2buf(fr, buf, album_idx_sz) == 0)
                    goto failure;

                data.album_index = buf;
                buf = (char *)buf + album_idx_sz;
                buf_size -= album_idx_sz;

                close(fr);

                /* sanity check loaded data */
                for (i = 0; i < data.album_ct; i++)
                {
                    album_idx = data.album_index[i].name_idx;
                    artist_idx = data.album_index[i].artist_idx;
                    if (album_idx >= (int) data.album_len ||
                        artist_idx >= (int) data.artist_len)
                    {
                        goto failure;
                    }
                }

                memcpy(pfi, &data, sizeof(struct pf_index_t));
                pfi->buf = buf;
                pfi->buf_sz = buf_size;

                qsort(pfi->album_index, pfi->album_ct,
                          sizeof(struct album_data), compare_albums);

                return 0;
            }
        }
    }

failure:
    /* Foreground only -- the background pass owns no screen, and a failed
     * load there simply means it goes on to build one. */
    if (!building_bg)
        splash(HZ/2, "Failed to load index");
    if (fr >= 0)
        close(fr);

    pfi->buf = bufstart;
    pfi->buf_sz = bufstart_sz;
    pfi->artist_ct = 0;
    pfi->album_ct = 0;
    return -1;

}

/* carousel_model.build_index for the album model: reuse the on-disk index when
 * it matches the current cache version, otherwise rebuild it (and persist it).
 * Returns SUCCESS or one of the ERROR_* codes. */
/* Reuse the saved index, or build one and persist it, into the caller's
 * struct and the caller's memory. Which memory is the only thing that varies
 * between a build the carousel asks for and one a background pass does. */
static int build_into(struct pf_index_t *target, void *buf, size_t buf_sz,
                      bool background)
{
    int ret = SUCCESS;

    mutex_lock(&build_mutex);
    idx_abort = false;

    /* Set inside the lock, and cleared before releasing it, so it describes
     * exactly who is building right now. Set it outside and a foreground build
     * can start while it still reads "background", then abort itself on the
     * next tick's idx_abort -- which closed the carousel mid-open. */
    building_bg = background;

    pfi = target;
    pfi->buf = buf;
    pfi->buf_sz = buf_sz;

    /* Scan will trigger when no file is found or the option was activated */
    if ((pf_cfg.cache_version != CACHE_VERSION) || (load_album_index() < 0))
    {
        ui_set_working(true);   /* show the "working" LED while (re)building */
        ret = create_album_index();
        ui_set_working(false);

        if (ret == 0)
        {
            pf_cfg.cache_version = CACHE_REBUILD;
            pf_config_save();
            /* Only the foreground caller may report this: the background pass
             * owns no screen, and there is nothing the user could do anyway --
             * the carousel will simply build it again when asked. */
            if (save_album_index() < 0 && !building_bg)
                splash(HZ, "Could not write index");
        }
    }

    pfi = NULL;
    building_bg = false;
    mutex_unlock(&build_mutex);
    return ret;
}

int album_index_build_into(struct pf_index_t *target, void *buf, size_t buf_sz)
{
    return build_into(target, buf, buf_sz, false);
}

/* Wait out a background build rather than restarting its work here.
 *
 * Interrupting it and rebuilding in the foreground was the original design,
 * from when both cost the same. They do not: this path redraws the status bar
 * for every album and the background one does not, which makes it several
 * times slower. So the quickest way to get an index is to let the pass that is
 * already running finish, then read what it wrote.
 *
 * Polls rather than blocking on the mutex, so the screen stays alive and can
 * be left. */
static int wait_for_background(void)
{
    if (!album_index_is_busy())
        return SUCCESS;

    /* Nothing on screen at all if it finishes promptly. */
    splash_progress_set_delay(HZ / 2);

    while (album_index_is_busy())
    {
        splash_progress(bg_done, bg_total > 0 ? bg_total : 1,
                        "%s", str(LANG_WAIT));

        if (get_action(CONTEXT_STD, HZ / 10) == ACTION_STD_CANCEL)
        {
            /* Leaving, not switching to the slow path: tell it to stop so it
             * is not still running behind whatever comes next. */
            idx_abort = true;
            return ERROR_USER_ABORT;
        }
    }

    return SUCCESS;
}

/* carousel_model.build_index: build into the engine's own index and the
 * buffer it has already claimed. */
int album_index_build(void)
{
    int ret = wait_for_background();
    if (ret != SUCCESS)
        return ret;

    return album_index_build_into(&pf_idx, pf_idx.buf, pf_idx.buf_sz);
}

/* ---------------------------------------------------------------------------
 * Building it in the background
 *
 * The index only goes stale when the database changes, so the work can be done
 * once after a scan settles rather than when someone opens the carousel and is
 * made to wait for it. The carousel is unchanged: it still asks for the index
 * the same way, and simply finds it already written most of the time.
 *
 * The pass runs on the same terms as the artwork cache thread it sits beside --
 * only while the database is usable and idle, and only once the entry count has
 * stopped moving, so a scan in progress is left alone.
 * ------------------------------------------------------------------------ */

#define IDX_STACK_SIZE (DEFAULT_STACK_SIZE + 0x2000)
static long idx_stack[IDX_STACK_SIZE / sizeof(long)];
static const char idx_thread_name[] = "albumidx";
static unsigned int idx_thread_id;
static struct event_queue idx_queue;

/* Enough to build a large library's index; the app buffer the carousel builds
 * into is 512K, and this is the background equivalent. Anything smaller just
 * fails with ERROR_BUFFER_FULL, which leaves the carousel to build inline as
 * it always has. */
#define IDX_BUILD_BUFSZ (384 * 1024)


bool album_index_is_busy(void)
{
    return building_bg;
}

/* The database entry count the last background build covered.
 *
 * Deliberately not pf_cfg.cache_version: that means "the on-disk formats are
 * current", is set by the carousel's prepare callback, and also gates the
 * slide placeholder's regeneration. Borrowing it here would both rebuild
 * forever (nothing sets it on this path) and quietly change what the carousel
 * does with it. This is the same marker the artwork cache keeps for the same
 * reason -- what was the library like when we last finished. */
#define IDX_DONE_FILE CACHE_PREFIX "/index_done.txt"

static int read_done_total(void)
{
    char buf[16];
    int total = -1;
    int fd = open(IDX_DONE_FILE, O_RDONLY);

    if (fd >= 0)
    {
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            total = atoi(buf);
        }
        close(fd);
    }
    return total;
}

static void write_done_total(int total)
{
    int fd = open(IDX_DONE_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd >= 0)
    {
        fdprintf(fd, "%d", total);
        close(fd);
    }
}

/* Is the saved index usable as it stands? Cheap: no allocation, no read. */
static bool saved_index_current(int total)
{
    if (read_done_total() != total)
        return false;

    int fd = open(ALBUM_INDEX, O_RDONLY);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

/* One background build, into memory of its own. The result is the file on
 * disk; the index it builds in RAM is thrown away, because the carousel will
 * read it back for itself when it needs it. */
static void background_build(int total)
{
    struct pf_index_t local;
    size_t bufsz = IDX_BUILD_BUFSZ;
    int handle;
    void *buf;
    int ret;
    const int old_version = pf_cfg.cache_version;

    /* The carousel makes this directory when it starts up; a build that runs
     * before it ever has would otherwise have nowhere to write the index. */
    if (!dir_exists(CACHE_PREFIX) && mkdir(CACHE_PREFIX) < 0)
        return;

    /* Ask outright rather than checking core_allocatable() first: that reports
     * space already free, and on a player with the audio buffer claimed there
     * is rarely any. core_alloc() shrinks what will shrink to make room, which
     * is the only way this ever gets memory. Failure just means try later. */
    handle = core_alloc(bufsz);
    if (handle <= 0)
        return;

    /* Pinned: a handle with no ops is movable, and the builder yields all the
     * way through -- to the database, and to be nice to playback. */
    buf = core_get_data_pinned(handle);

    memset(&local, 0, sizeof(local));
    bg_done = bg_total = 0;

    ret = build_into(&local, buf, bufsz, true);

    bg_done = bg_total = 0;

    core_put_data_pinned(buf);
    core_free(handle);

    /* Only on a clean finish: an aborted or failed pass must be retried, not
     * recorded as covering this library. */
    if (ret == SUCCESS)
    {
        /* Finish the handshake the carousel would otherwise complete for us.
         *
         * The builder leaves cache_version at CACHE_REBUILD, and it is the
         * carousel's prepare callback that later marks it current -- so a
         * background build that stopped here would leave the flag saying
         * "stale" and the carousel would rebuild everything we just did.
         *
         * That flag also decides whether the placeholder slide is regenerated,
         * which is not ours to skip: if it was out of date, drop the file so
         * the carousel makes a new one from its absence instead. */
        if (old_version != CACHE_VERSION)
            remove(EMPTY_SLIDE);

        pf_cfg.cache_version = CACHE_VERSION;
        pf_config_save();

        write_done_total(total);
    }
}

static void idx_thread(void)
{
    struct queue_event ev;
    int prev_total = -1;    /* entry count seen last tick (stability check) */

    while (1)
    {
        queue_wait_w_tmo(&idx_queue, &ev, HZ * 5);

        switch (ev.id)
        {
            case SYS_USB_CONNECTED:
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                usb_wait_for_disconnect(&idx_queue);
                prev_total = -1;   /* the library may have changed under us */
                break;

            case SYS_TIMEOUT:
            {
                if (!tagcache_is_usable() || tagcache_is_busy())
                {
                    prev_total = -1;
                    break;
                }

                int total = tagcache_get_stat()->total_entries;
                if (total != prev_total)
                {
                    /* Still moving -- wait for it to settle before spending
                     * a scan of our own on a moving target. */
                    prev_total = total;
                    break;
                }

                if (saved_index_current(total))
                    break;

                background_build(total);
                break;
            }
        }
    }
}

void album_index_init(void)
{
    mutex_init(&build_mutex);
    queue_init(&idx_queue, true);
    idx_thread_id = create_thread(idx_thread, idx_stack, sizeof(idx_stack), 0,
                                  idx_thread_name IF_PRIO(, PRIORITY_BACKGROUND)
                                  IF_COP(, CPU));
    (void)idx_thread_id;
}

void album_index_invalidate(void)
{
    /* Forget what the last pass covered, so the next tick rebuilds. Used by
     * the menu's rebuild/update, which otherwise only reach the carousel's own
     * build and would leave the background pass thinking it was up to date. */
    remove(IDX_DONE_FILE);
}
