/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/albumart.c
 * Copyright (C) 2007 Nicolas Pennequin
 * GNU General Public License (version 2+)
 *
 * Finds the image file standing for a track's cover: one in /Artwork/Covers
 * named for its album, else one named after the track, cover.* or folder.*
 * in its folder, or in the folder above a disc folder.
 ****************************************************************************/

#include <stdio.h>
#include "string-extra.h"
#include "system.h"
#include "albumart.h"
#include "metadata.h"
#include "rbpaths.h"
#include "audio/buffering.h"
#include "dir.h"
#include "dircache.h"
#include "system/strutil.h"
#include "pathfuncs.h"
#include "settings/settings.h"
#include "art_cache.h"
#include "settings/settings.h"
#include "screens/playback/wps.h"
#include "draw/image_load.h"
#include "draw/gif_load.h"
#include "draw/webp_load.h"
#include "art_name.h"
#include "database/db_spoken.h"

/* Define LOGF_ENABLE to enable logf output in this file */
/*#define LOGF_ENABLE*/
#include "logf.h"

#define USE_JPEG_COVER


/* Whether an image the search has found can be drawn. The JPEG decoder takes
 * baseline and progressive Huffman-coded images only, the PNG reader no
 * very wide ones and the WebP reader no lossless ones; one they cannot read
 * must not stop the search: an arithmetic-coded cover.jpg would otherwise
 * hide a folder.jpg beside it, which the search only reaches later. Reads a
 * JPEG's markers up to the frame header, or a PNG's, GIF's or WebP's
 * headers, by content whatever the name, and leaves anything it cannot place
 * for the decoder to judge. */
bool art_file_usable(const char *path)
{
    unsigned char b[4];
    const char *ext = strrchr(path, '.');
    int fd, i;
    bool usable = true;

    if (!file_exists(path))
        return false;
    if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg")
                 && strcasecmp(ext, ".png") && strcasecmp(ext, ".gif")
                 && strcasecmp(ext, ".webp")))
        return true;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    i = read(fd, b, 2);
    if (i == 2 && b[0] == 0x89 && b[1] == 'P')
        usable = png_usable(fd);
    else if (i == 2 && b[0] == 'G' && b[1] == 'I')
        usable = gif_usable(fd);
    else if (i == 2 && b[0] == 'R' && b[1] == 'I')
        usable = webp_usable(fd);
    else if (i == 2 && b[0] == 0xff && b[1] == 0xd8)
    {
        for (i = 0; i < 64 && read(fd, b, 4) == 4 && b[0] == 0xff; i++)
        {
            int marker = b[1], seglen = (b[2] << 8) | b[3];

            if (marker == 0xc0 || marker == 0xc2)
                break;                          /* baseline, progressive */
            if (marker >= 0xc1 && marker <= 0xcf
                && marker != 0xc4 && marker != 0xcc)
            {
                usable = false;                 /* arithmetic and the rest */
                break;
            }
            if (seglen < 2 || lseek(fd, seglen - 2, SEEK_CUR) < 0)
                break;
        }
    }
    close(fd);
    return usable;
}

static const char * const extensions[] = { "jpeg", "jpg", "bmp", "png", "gif",
                                           "webp" };
static const unsigned char extension_lens[] = { 4, 3, 3, 3, 3, 4 };
/* Try checking for several file extensions, return true if a file is found and
 * leaving the path modified to include the matching extension.
 *
 * Callers hand in a path ending in '.' and len counting that dot, so each
 * candidate extension is written straight over the terminator in turn rather
 * than rebuilding the whole path. path must therefore have room for the
 * longest extension plus a NUL beyond len.
 */
static bool try_exts(char *path, int len)
{
    int i;
    for (i = 0; i < (int)ARRAYLEN(extensions); i++)
    {
        if (extension_lens[i] + len > MAX_PATH)
            continue;
        strcpy(path + len, extensions[i]);
        if (art_file_usable(path))
            return true;
    }
    return false;
}

/* An image called 'stem' plus one of the extensions in the folder [dir,
 * dir + dirlen), the length counting its slash, into p */
static bool try_named(char *p, const char *dir, size_t dirlen,
                      const char *stem, size_t stemlen)
{
    if (dirlen + stemlen + 1 + 4 >= MAX_PATH)
        return false;
    memcpy(p, dir, dirlen);
    memcpy(p + dirlen, stem, stemlen);
    p[dirlen + stemlen] = '.';
    p[dirlen + stemlen + 1] = '\0';
    return try_exts(p, dirlen + stemlen + 1);
}

bool art_image_ext(const char *dot)
{
    int i;

    if (!dot || *dot != '.')
        return false;
    for (i = 0; i < (int)ARRAYLEN(extensions); i++)
        if (!strcasecmp(dot + 1, extensions[i]))
            return true;
    return false;
}

/* The image for the file at 'path' in its folder: one named for the file,
 * then, unless 'own_only', cover.* and folder.* there or above a disc
 * folder */
static bool folder_file(const char *path, char *buf, int buflen,
                        bool own_only)
{
    char p[MAX_PATH + 8];
    const char *sep = strrchr(path, '/');
    const char *name, *dot, *start;
    size_t dirlen;

    if (!sep)
        return false;
    dirlen = sep - path + 1;
    name = sep + 1;
    dot = strrchr(name, '.');
    if (try_named(p, path, dirlen, name, dot ? (size_t)(dot - name)
                                             : strlen(name)))
        goto found;
    if (own_only)
        return false;
    if (try_named(p, path, dirlen, "cover", 5)
        || try_named(p, path, dirlen, "folder", 6))
        goto found;

    for (start = sep; start > path && start[-1] != '/'; start--)
        ;
    if (start - 1 > path && is_disc_folder(start, sep)
        && (try_named(p, path, start - path, "cover", 5)
            || try_named(p, path, start - path, "folder", 6)))
        goto found;
    return false;

found:
    strmemccpy(buf, p, buflen);
    logf("Album art found: %s", p);
    return true;
}

bool art_folder_file(const char *path, char *buf, int buflen)
{
    return folder_file(path, buf, buflen, false);
}

/* Whether the folder of the file at 'path' is named for 'person', matched as
 * the art cache matches names: a book's cover.* and folder.* there are then
 * its author's picture, not its cover */
static bool in_person_folder(const char *path, const char *person)
{
    const char *end = strrchr(path, '/'), *start;
    unsigned int k1, a1, k2 = art_name_hash(person);
    unsigned int a2 = art_name_alt_hash(person);

    if (!end)
        return false;
    for (start = end; start > path && start[-1] != '/'; start--)
        ;
    k1 = art_name_hash_n(start, end - start);
    a1 = art_name_alt_hash_n(start, end - start);
    return (k1 && (k1 == k2 || k1 == a2)) || (a1 && (a1 == k2 || a1 == a2));
}

bool art_covers_find(const char *artist, const char *title, char *buf,
                     int buflen)
{
    unsigned int kw = art_name_work_hash(artist, title);
    unsigned int kt = art_name_hash(title);
    bool found = false, by_title = false;
    DIR *d;
    struct dirent *e;

    if (!kt || !(d = opendir(ART_COVERS_DIR)))
        return false;
    while (!found && (e = readdir(d)))
    {
        const char *dot = strrchr(e->d_name, '.');
        unsigned int k;
        char p[MAX_PATH];

        if (e->d_name[0] == '.' || !art_image_ext(dot))
            continue;
        k = art_name_hash_n(e->d_name, dot - e->d_name);
        if (k != kw && (by_title || k != kt))
            continue;
        snprintf(p, sizeof(p), ART_COVERS_DIR "/%s", e->d_name);
        if (!art_file_usable(p))
            continue;
        strmemccpy(buf, p, buflen);
        if (k == kw)
            found = true;
        else
            by_title = true;    /* unless a later one names the artist too */
    }
    closedir(d);
    return found || by_title;
}

bool art_cover_file(const struct mp3entry *id3, char *buf, int buflen)
{
    if (!id3 || !buf || buflen <= 0)
        return false;
    const char *artist = id3->albumartist && id3->albumartist[0]
                         ? id3->albumartist : id3->artist;

    if (strcmp(id3->path, "No file!") == 0)
        return false;
    return art_covers_find(artist, id3->album, buf, buflen)
           || folder_file(id3->path, buf, buflen,
                          artist && artist[0]
                          && db_spoken_is_book_genre(id3->genre_string)
                          && in_person_folder(id3->path, artist));
}

/* One source, if the track has it. */
static bool try_source(const struct mp3entry *id3, int kind, bool as_stored,
                       struct albumart_source *src)
{
    switch (kind)
    {
    case AA_SOURCE_EMBEDDED:
        if (!id3->has_embedded_albumart || id3->albumart.size <= 0)
            return false;
        /* Unsynchronised or base64 bytes are not a JPEG until decoded */
        if (as_stored ? id3->albumart.type != AA_TYPE_JPG
                      : !image_embedded_usable(id3->albumart.type))
            return false;
        strmemccpy(src->path, id3->path, sizeof(src->path));
        src->pos = id3->albumart.pos;
        src->size = id3->albumart.size;
        break;
    case AA_SOURCE_FILE:
    {
        if (!art_cover_file(id3, src->path, sizeof(src->path)))
            return false;
        const char *dot = strrchr(src->path, '.');
        if (as_stored && !(dot && (!strcasecmp(dot, ".jpg") ||
                                   !strcasecmp(dot, ".jpeg"))))
            return false;
        src->pos = 0;
        src->size = -1;
        break;
    }
    case AA_SOURCE_CACHE:
    {
        /* Raw thumbnail pixels: as_stored takes them to encode itself */
        const int size_index = art_cache_size_index("wps");
        bool fallback;
        if (size_index < 0
            || !art_cache_lookup_hash(art_cache_track_key(id3->path),
                                      size_index, src->path,
                                      sizeof(src->path), &fallback)
            || fallback)
            return false;
        src->pos = 0;
        src->size = -1;
        break;
    }
    default:
        return false;
    }
    src->kind = kind;
    return true;
}

bool albumart_find_source(const struct mp3entry *id3, int preference,
                          bool as_stored, struct albumart_source *src)
{
    static const int order[][3] = {
        [AA_PREFER_EMBEDDED] =
            { AA_SOURCE_EMBEDDED, AA_SOURCE_FILE, AA_SOURCE_CACHE },
        [AA_PREFER_IMAGE_FILE] =
            { AA_SOURCE_FILE, AA_SOURCE_EMBEDDED, AA_SOURCE_CACHE },
        [AA_PREFER_CACHE] =
            { AA_SOURCE_CACHE, AA_SOURCE_EMBEDDED, AA_SOURCE_FILE },
    };

    src->kind = AA_SOURCE_NONE;
    if (!id3 || preference <= AA_OFF || preference > AA_PREFER_CACHE)
        return false;
    for (int i = 0; i < 3; i++)
        if (try_source(id3, order[preference][i], as_stored, src))
            return true;
    return false;
}
