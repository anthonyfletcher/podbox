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

static void draw_progressbar(int step, int count, char *msg);

static void draw_progressbar(int step, int count, char *msg)
{
    (void)step; (void)count; (void)msg;
    sb_skin_update(SCREEN_MAIN, true);
    skin_flush_dirty();
}

static bool progress_cancel(int step, int count, char *msg)
{
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
    pf_idx.album_index[idx].name_idx = name_idx;
    pf_idx.album_index[idx].seek = album_seek;
    pf_idx.album_index[idx].artist_idx = artist_idx;
    pf_idx.album_index[idx].artist_seek = artist_seek;
    pf_idx.album_index[idx].year = 0;
}

static inline void write_album_entry(struct tagcache_search *tcs,
                                     int name_idx, unsigned int len)
{
    write_album_index(-pf_idx.album_ct, name_idx, tcs->result_seek, 0, -1);
    pf_idx.album_len += len;
    pf_idx.album_ct++;

    if (pf_idx.album_untagged_seek == -1 && strcmp(UNTAGGED, tcs->result) == 0)
    {
        pf_idx.album_untagged_idx = name_idx;
        pf_idx.album_untagged_seek = tcs->result_seek;
    }
}

static void write_artist_entry(struct tagcache_search *tcs,
                               int name_idx, unsigned int len)
{
    pf_idx.artist_index[-pf_idx.artist_ct].name_idx = name_idx;
    pf_idx.artist_index[-pf_idx.artist_ct].seek = tcs->result_seek;
    pf_idx.artist_len += len;
    pf_idx.artist_ct++;
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
         * pf_idx.buf on any library large enough to fill it. */
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
    int album_count = pf_idx.album_ct; /* store existing count */
    int total_count = pf_idx.album_ct + pf_idx.artist_ct * 2;
    long seek;
    int last, final, retry;
    int i, j;
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, total_count, STR_STEP_INDEXING_UNTAGGED);

    /* search tagcache for all <untagged> albums & save the albumartist seek pos */
    if (tagcache_search(tcs, tag_albumartist))
    {
        tagcache_search_add_filter(tcs, tag_album, pf_idx.album_untagged_seek);

        while (tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
        {
            if (progress_cancel(pf_idx.album_ct, total_count, STR_STEP_INDEXING_UNTAGGED))
            {
                tagcache_search_finish(tcs);
                return ERROR_USER_ABORT;
            }

            if (tcs->result_seek ==
                pf_idx.album_index[-(pf_idx.album_ct - 1)].artist_seek)
                continue;

            if (sizeof(struct album_data) > *bufsz)
            {
                /* not enough memory */
                ret = ERROR_BUFFER_FULL;
                break;
            }

            *bufsz -= sizeof(struct album_data);
            write_album_index(-pf_idx.album_ct, pf_idx.album_untagged_idx,
                               pf_idx.album_untagged_seek, -1, tcs->result_seek);

            pf_idx.album_ct++;
        }
        tagcache_search_finish(tcs);

        if (ret == SUCCESS) {
            draw_progressbar(0, pf_idx.album_ct, STR_STEP_INDEXING_UNTAGGED);

            last = 0;
            final = pf_idx.artist_ct;
            retry = 0;

            /* map the artist_seek position to the artist name index */
            for (j = album_count; j < pf_idx.album_ct; j++)
            {
                if (progress_cancel(j, pf_idx.album_ct, STR_STEP_INDEXING_UNTAGGED))
                    return ERROR_USER_ABORT;

                seek = pf_idx.album_index[-j].artist_seek;

    retry_artist_lookup:
                retry++;
                for (i = last; i < final; i++)
                {
                    if (seek == pf_idx.artist_index[i].seek)
                    {
                        int idx = pf_idx.artist_index[i].name_idx;
                        pf_idx.album_index[-j].artist_idx = idx;
                        last = i; /* last match, start here next loop */
                        final = pf_idx.artist_ct;
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
    pf_idx.artist_index = ((struct artist_data *)(*bufsz + (char *) *buf)) - 1;
    pf_idx.artist_ct = 0;
    pf_idx.artist_len = 0;
    /* artist names starts at beginning of buf */
    pf_idx.artist_names = *buf;

    tagcache_search(tcs, tag_albumartist);
    res = get_tcs_search_res(ePFS_ARTIST, tcs, &(*buf), bufsz);
    tagcache_search_finish(tcs);
    if (res < SUCCESS)
        return res;

    /* finalize the artist index */
    ALIGN_BUFFER(*buf, *bufsz, alignof(struct artist_data));
    tmp_artist = (struct artist_data*)*buf;
    for (i = pf_idx.artist_ct - 1; i >= 0; i--)
        tmp_artist[i] = pf_idx.artist_index[-i];

    pf_idx.artist_index = tmp_artist;
    /* move buf ptr to end of artist_index */
    *buf = pf_idx.artist_index + pf_idx.artist_ct;

    if (res == SUCCESS)
    {
        if (pf_idx.artist_ct > 0)
            res = pf_idx.artist_ct;
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
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUM_YEAR);
    for (int album_idx = 0; album_idx < pf_idx.album_ct; album_idx++)
    {
        /* Prevent idle poweroff */
        reset_poweroff_timer();

        if (progress_cancel(album_idx, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUM_YEAR))
            return ERROR_USER_ABORT;

        int album_year = 0;

        if (tagcache_search(&tcs, tag_year))
        {
            tagcache_search_add_filter(&tcs, tag_album,
                                       pf_idx.album_index[album_idx].seek);

            if (pf_idx.album_index[album_idx].artist_idx >= 0)
                tagcache_search_add_filter(&tcs, tag_albumartist,
                    pf_idx.album_index[album_idx].artist_seek);

            while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz)) {
                int track_year = tagcache_get_numeric(&tcs, tag_year);
                if (track_year > album_year)
                    album_year = track_year;
            }
        }
        tagcache_search_finish(&tcs);

        pf_idx.album_index[album_idx].year = album_year;
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
    void *buf = pf_idx.buf;
    size_t buf_size = pf_idx.buf_sz;

    struct album_data* tmp_album;

    int i, j, last, final, retry, res;

    ALIGN_BUFFER(buf, buf_size, sizeof(long));

    /* Artists */
    res = build_artist_index(&tcs, &buf, &buf_size);
    if (res < SUCCESS)
        return res;

    /* Albums */
    pf_idx.album_ct = 0;
    pf_idx.album_len =0;
    pf_idx.album_untagged_idx = -1;
    pf_idx.album_untagged_seek = -1;

    /* album_index starts at end of buf it will be rearranged when finalized */
    pf_idx.album_index = ((struct album_data *)(buf_size + (char *)buf)) - 1;
    /* album_names starts at the beginning of buf */
    pf_idx.album_names = buf;

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
    for (i = pf_idx.album_ct - 1; i >= 0; i--)
        tmp_album[i] = pf_idx.album_index[-i];

    pf_idx.album_index = tmp_album;
    /* move buf ptr to end of album_index */
    buf = pf_idx.album_index + pf_idx.album_ct;

    /* Assign indices */
    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUMS);
    for (j = 0; j < pf_idx.album_ct; j++)
    {
        /* Prevent idle poweroff */
        reset_poweroff_timer();

        if (progress_cancel(j, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUMS))
            return ERROR_USER_ABORT;

        if (pf_idx.album_index[j].artist_seek >= 0) { continue; }

        tagcache_search(&tcs, tag_albumartist);
        tagcache_search_add_filter(&tcs, tag_album, pf_idx.album_index[j].seek);

        last = 0;
        final = pf_idx.artist_ct;
        retry = 0;
        if (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
        {

retry_artist_lookup:
            retry++;
            for (i = last; i < final; i++)
            {
                if (tcs.result_seek == pf_idx.artist_index[i].seek)
                {
                    int idx = pf_idx.artist_index[i].name_idx;
                    pf_idx.album_index[j].artist_idx = idx;
                    pf_idx.album_index[j].artist_seek = tcs.result_seek;
                    last = i; /* last match, start here next loop */
                    final = pf_idx.artist_ct;
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
    qsort(pf_idx.album_index, pf_idx.album_ct,
              sizeof(struct album_data), compare_album_artists);

    splash_progress_set_delay(HZ / 2);
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_REMOVING_DUPLICATES);
    /* mark duplicate albums for deletion */
    for (i = 0; i < pf_idx.album_ct - 1; i++) /* -1 don't check last entry */
    {
        /* Prevent idle poweroff */
        reset_poweroff_timer();

        if (progress_cancel(i, pf_idx.album_ct, STR_STEP_REMOVING_DUPLICATES))
            return ERROR_USER_ABORT;

        int idxi = pf_idx.album_index[i].artist_idx;
        int seeki = pf_idx.album_index[i].seek;

        for (j = i + 1; j < pf_idx.album_ct; j++)
        {
            if (idxi > 0 &&
            idxi == pf_idx.album_index[j].artist_idx &&
            seeki == pf_idx.album_index[j].seek)
            {
                pf_idx.album_index[j].artist_idx = -1;
            }
            else
            {
                i = j - 1;
                break;
            }
        }
    }

    /* now fix the album list order */
    qsort(pf_idx.album_index, pf_idx.album_ct,
              sizeof(struct album_data), compare_album_artists);

    /* remove any extra untagged albums
     * extra space is orphaned till restart */
    pf_idx.album_index += pf_idx.album_untagged_idx + 1;
    pf_idx.album_ct -= pf_idx.album_untagged_idx + 1;

    pf_idx.buf = buf;
    pf_idx.buf_sz = buf_size;
    pf_idx.artist_index = 0;

    qsort(pf_idx.album_index, pf_idx.album_ct,
                          sizeof(struct album_data), compare_albums);

    return (pf_idx.album_ct > 0) ? 0 : ERROR_NO_ALBUMS;
}

/*Saves the album index into a binary file to be recovered the
 next time PictureFlow is launched*/

static int save_album_index(void){
    int fd = creat(ALBUM_INDEX,0666);

    struct pf_index_t data;
    memcpy(&data, &pf_idx, sizeof(struct pf_index_t));

    if(fd >= 0)
    {
        memcpy(&data.header, INDEX_HDR, sizeof(pf_idx.header));

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

    void *bufstart = pf_idx.buf;
    unsigned int bufstart_sz = pf_idx.buf_sz;

    void* buf = pf_idx.buf;
    size_t buf_size = pf_idx.buf_sz;

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

                memcpy(&pf_idx, &data, sizeof(struct pf_index_t));
                pf_idx.buf = buf;
                pf_idx.buf_sz = buf_size;

                qsort(pf_idx.album_index, pf_idx.album_ct,
                          sizeof(struct album_data), compare_albums);

                return 0;
            }
        }
    }

failure:
    splash(HZ/2, "Failed to load index");
    if (fr >= 0)
        close(fr);

    pf_idx.buf = bufstart;
    pf_idx.buf_sz = bufstart_sz;
    pf_idx.artist_ct = 0;
    pf_idx.album_ct = 0;
    return -1;

}

/* carousel_model.build_index for the album model: reuse the on-disk index when
 * it matches the current cache version, otherwise rebuild it (and persist it).
 * Returns SUCCESS or one of the ERROR_* codes. */
int album_index_build(void)
{
    int ret = SUCCESS;

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
            if (save_album_index() < 0)
                splash(HZ, "Could not write index");
        }
    }
    return ret;
}
