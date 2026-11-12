#!/usr/bin/env python3
"""Actual null-safe cover lookup and playback-report generation invalidation."""
from host import OUT, run, CC, function
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
struct mp3entry { const char *albumartist, *artist, *album, *genre_string; char path[260]; };
static bool art_covers_find(const char *a, const char *t, char *b, int n)
{ (void)a; (void)t; (void)b; (void)n; return false; }
static bool folder_file(const char *p, char *b, int n, bool own)
{ (void)p; (void)b; (void)n; (void)own; return false; }
static bool db_spoken_is_book_genre(const char *g) { (void)g; return false; }
static bool in_person_folder(const char *p, const char *a) { (void)p; (void)a; return false; }
#define ART_SLOTS 8
static unsigned art_key[ART_SLOTS], hue_key[2][2], current_generation;
static bool cache_busy;
static unsigned art_cache_generation(void) { return current_generation; }
static bool art_cache_is_busy(void) { return cache_busy; }
#define MAX_PATH 260
#define TREE_AA_MAX_DIM 44
#define TREE_AA_SLOTS 2
#define TREE_AA_NO_HASH 2
#define FORMAT_NATIVE 1
#define MIN(a,b) ((a) < (b) ? (a) : (b))
struct dim { int width, height; };
struct bitmap { int width, height, format; unsigned char *data; };
struct img_filter { int n; };
typedef unsigned browser_art_hash_fn(int, void *);
static struct { int item, dim, w, h; uint32_t filter_hash; } browser_aa_slot[TREE_AA_SLOTS];
static int browser_aa_no_hash[TREE_AA_NO_HASH], browser_aa_no_hash_next;
static int browser_aa_dim, browser_aa_size_idx, browser_aa_victim, browser_aa_handle;
static struct bitmap browser_aa_bm;
static int resets;
static void browser_aa_reset(void)
{ resets++; browser_aa_slot[0].item = -1; browser_aa_no_hash[0] = -1; }
static bool browser_aa_ready(int want) { (void)want; return false; }
static bool tagcache_is_in_ram(void) { return true; }
static bool tagcache_ram_refused(void) { return false; }
static bool art_cache_lookup_hash(unsigned k, int i, char *p, int n, bool *f)
{ (void)k; (void)i; (void)p; (void)n; (void)f; return false; }
static bool browser_aa_load(const char *p, int s, const struct img_filter *f)
{ (void)p; (void)s; (void)f; return false; }
static void *core_get_data(int h) { (void)h; return NULL; }
static size_t browser_aa_slot_bytes(void) { return 1; }
static unsigned get_hash(int item, void *data) { (void)item; (void)data; return 0; }
'''
code += function('apps-ipod/metadata/albumart.c', 'art_cover_file')
code += function('apps-ipod/viewers/playback_viewer/pv_row.c', 'art_resolved')
code += function('apps-ipod/screens/browse/browser.c', 'browser_art_row')
code += r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "null"))
    {
        char out[260];
        struct mp3entry id3 = {0};
        assert(!art_cover_file(NULL, out, sizeof(out)));
        assert(!art_cover_file(&id3, NULL, 0));
    }
    else if (!strcmp(argv[1], "browser"))
    {
        browser_aa_slot[0].item = browser_aa_no_hash[0] = 17;
        cache_busy = true;
        for (int i = 0; i < 3; i++)
        {
            current_generation++;
            browser_art_row(17, NULL, get_hash, NULL, NULL, 0);
            assert(!resets && browser_aa_slot[0].item == 17 && browser_aa_no_hash[0] == 17);
        }
        cache_busy = false;
        browser_art_row(17, NULL, get_hash, NULL, NULL, 0);
        assert(resets == 1);
        browser_art_row(17, NULL, get_hash, NULL, NULL, 0);
        assert(resets == 1);
    }
    else
    {
        art_key[0] = hue_key[0][0] = 17;
        assert(art_resolved(17));
        cache_busy = true;
        for (int i = 0; i < 3; i++)
        {
            current_generation++;
            assert(art_resolved(17) && hue_key[0][0] == 17);
        }
        cache_busy = false;
        assert(!art_resolved(17));
        assert(!hue_key[0][0]);
    }
    return 0;
}
'''
source = OUT / 'consumers.c'
source.write_text(code)
run([CC, '-std=gnu11', '-g', '-O1', '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', source, '-o', OUT / 'consumers'], 'compile-consumers')
failed = []
for mode in ['null', 'report', 'browser']:
    result = run([OUT / 'consumers', mode], mode, check=False)
    print(mode, 'PASS' if result.returncode == 0 else 'FAIL')
    if result.returncode:
        failed.append(mode)
assert not failed, failed
