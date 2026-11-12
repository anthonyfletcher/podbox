#!/usr/bin/env python3
"""Production source selection: lazy iTunes indexing and non-destructive failure."""
from host import ROOT, OUT, run, CC, function
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
enum { ART_SOURCE_FILES, ART_SOURCE_FILES_FIRST, ART_SOURCE_EMBEDDED_FIRST, ART_SOURCE_EMBEDDED };
enum { AA_STAMP_NONE, AA_STAMP_EMBEDDED, AA_STAMP_CLAIMED, AA_STAMP_NO_EMBED };
enum { AA_ITUNES_UNTRIED, AA_ITUNES_ABSENT, AA_ITUNES_READY, AA_ITUNES_FAILED };
static int aa_itunes_state;
struct { int art_cache_album_source; } global_settings;
struct aa_stamp { unsigned key, stamp; };
struct aa_work { const char *artist, *title; bool named, folder; };
struct itunes_art { char path[260]; unsigned pos, width, height, stride; };
struct aa_src { const char *path; long emb_pos; unsigned long emb_size; int emb_flags; const struct itunes_art *itunes; };
static struct itunes_art aa_itunes;
static bool aa_check_all = true, aa_reread_tags = true, aa_checking;
static char aa_artpath[260];
static const char *aa_miss, *aa_miss_src;
static int central, selected, opens, deletes;
static bool cached, database_present = true, open_ok = true, ready;
static void aa_note(const char *fmt, ...) { (void)fmt; }
static bool aa_thumbs_exist(unsigned key) { (void)key; return cached; }
static bool aa_covers_central(const char *a, const char *t)
{ (void)a; (void)t; strcpy(aa_artpath, "/central.jpg"); return central; }
static bool aa_book_file(const char *p) { (void)p; return false; }
static bool aa_find_file(const char *p) { (void)p; return false; }
static bool aa_find_embedded(const char *p, struct aa_src *s) { (void)p; (void)s; return false; }
static bool aa_check_abort(void) { return false; }
static bool tagcache_is_busy(void) { return false; }
static void aa_remove_thumbs(unsigned key) { (void)key; deletes++; cached = false; }
static unsigned aa_art_stamp(const char *p) { (void)p; return 42; }
static bool file_exists(const char *p) { (void)p; return database_present; }
static bool itunes_art_open(bool (*stop)(void))
{ (void)stop; opens++; ready = open_ok; return ready; }
static bool itunes_art_find(const char *p, struct itunes_art *a)
{ (void)p; if (!ready) return false; strcpy(a->path, "/iPod_Control/Artwork/F1061_1.ithmb"); a->pos = 10; return true; }
static bool aa_fill(const struct aa_src *s, unsigned stamp, unsigned key,
                    struct aa_stamp *slot, void *buf, size_t size, bool *abort)
{
    (void)stamp; (void)key; (void)slot; (void)buf; (void)size; (void)abort;
    selected = s->itunes ? 2 : 1;
    cached = true;
    return true;
}
#define ITUNES_ARTWORK_DB "/iPod_Control/Artwork/ArtworkDB"
'''
code += function('apps-ipod/metadata/art_cache.c', 'aa_leaf')
if 'static bool aa_find_itunes(' in (ROOT / 'apps-ipod/metadata/art_cache.c').read_text():
    code += function('apps-ipod/metadata/art_cache.c', 'aa_find_itunes')
code += function('apps-ipod/metadata/art_cache.c', 'aa_cache_dir')
code += r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    bool aborted = false;
    struct aa_stamp slot = {12, 42};
    struct aa_work work = {"Artist", "Album", false, true};
    global_settings.art_cache_album_source = ART_SOURCE_FILES_FIRST;
    if (!strcmp(argv[1], "primary"))
    {
        central = true;
        open_ok = false;
        assert(aa_cache_dir("/song.mp3", 12, &slot, &work, NULL, 0, &aborted));
        assert(selected == 1 && !opens && !aborted);
    }
    else if (!strcmp(argv[1], "failure") || !strcmp(argv[1], "check-failure"))
    {
        cached = true;
        open_ok = false;
        aa_checking = !strcmp(argv[1], "check-failure");
        assert(!aa_cache_dir("/song.mp3", 12, &slot, &work, NULL, 0, &aborted));
        assert(aborted && cached && !deletes && slot.stamp == 42 && opens == 1);
        aborted = false;
        assert(!aa_cache_dir("/song.mp3", 12, &slot, &work, NULL, 0, &aborted));
        assert(aborted && cached && !deletes && opens == 1);
        /* A later job retries the index rather than retaining the failure. */
        aa_itunes_state = AA_ITUNES_UNTRIED;
        open_ok = true;
        aborted = false;
        assert(aa_cache_dir("/song.mp3", 12, &slot, &work, NULL, 0, &aborted));
        assert(selected == 2 && opens == 2 && !aborted);
    }
    else if (!strcmp(argv[1], "files"))
    {
        global_settings.art_cache_album_source = ART_SOURCE_FILES;
        assert(!aa_cache_dir("/song.mp3", 12, &slot, &work, NULL, 0, &aborted));
        assert(!opens && !aborted);
    }
    else if (!strcmp(argv[1], "absent"))
    {
        cached = true;
        database_present = false;
        assert(!aa_cache_dir("/song.mp3", 12, &slot, &work, NULL, 0, &aborted));
        assert(!opens && !aborted && deletes == 1);
    }
    else
    {
        aa_checking = !strcmp(argv[1], "check-success");
        cached = aa_checking;
        assert(aa_cache_dir("/Books/book.m4b", 12, &slot, &work, NULL, 0, &aborted));
        assert(selected == 2 && opens == 1 && !aborted);
        assert(deletes == (int)aa_checking);
        aa_checking = false;
        assert(aa_cache_dir("/song.mp3", 12, &slot, &work, NULL, 0, &aborted));
        assert(opens == 1);
    }
    return 0;
}
'''
path = OUT / 'cache.c'
path.write_text(code)
run([CC, '-std=gnu11', '-g', '-O1', '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', path, '-o', OUT / 'cache'], 'compile-cache')
failed = []
for mode in ['fallback', 'primary', 'files', 'absent', 'failure', 'check-failure', 'check-success']:
    result = run([OUT / 'cache', mode], mode, check=False)
    print(mode, 'PASS' if result.returncode == 0 else 'FAIL', flush=True)
    if result.returncode:
        failed.append(mode)
# Jobs must leave indexing to the first fallback, not perform eager startup I/O.
for name in ['aa_run_pass', 'aa_run_check']:
    body = function('apps-ipod/metadata/art_cache.c', name)
    if 'itunes_art_open(' in body:
        failed.append('eager-' + name)
# Check-a-file must preserve its existing album/book images until source resolution.
for name, deletion in [('aa_run_check', 'aa_remove_thumbs(dh)'), ('aa_check_audiobook', 'aa_remove_thumbs(bh)')]:
    if deletion in function('apps-ipod/metadata/art_cache.c', name):
        failed.append('early-delete-' + name)
assert not failed, failed
