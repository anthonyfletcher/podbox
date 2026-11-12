#!/usr/bin/env python3
"""Production offer/sleep/retry functions with deterministic queue and memory ports."""
from host import ROOT, OUT, run, CC, function
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MAX_PATH 260
#define ART_CACHE_NUM_SIZES 3
#define HZ 100
#define AA_EVENT_OFFER 1
#define AA_STAMP_EMBEDDED 1
#define ART_SOURCE_FILES 0
#define ART_SOURCE_FILES_FIRST 1
struct aa_src { const char *path; long emb_pos; unsigned long emb_size; int emb_flags; const void *itunes; };
struct timeout { int n; };
static struct { int n; } art_cache_task;
static struct { char path[MAX_PATH]; long pos; unsigned long size; int flags; unsigned serial, retries; } aa_offer;
static struct { int art_cache_album_source; } global_settings = {ART_SOURCE_FILES_FIRST};
static struct mp3entry { char path[MAX_PATH]; bool has_embedded_albumart; struct { int type; long pos; unsigned long size; } albumart; } id3;
static struct mp3entry *audio_current_track(void) { return &id3; }
static bool image_embedded_usable(int type) { (void)type; return true; }
static bool aa_offer_posted, aa_io_failed, asleep, supersede, cache_busy;
static unsigned generation;
static int reloads;
static unsigned art_cache_generation(void) { return generation; }
static bool art_cache_is_busy(void) { return cache_busy; }
static void offer_frame(void);
static struct timeout aa_offer_tmo;
static int (*poll_fn)(struct timeout *);
static int posts, generated, memory_checks;
static size_t memory;
static char aa_check_path[MAX_PATH], aa_out_path[MAX_PATH];
static bool storage_disk_is_active(void) { return !asleep; }
static void timeout_register(struct timeout *t, int (*fn)(struct timeout *), int delay, intptr_t data)
{ (void)t; (void)delay; (void)data; poll_fn = fn; }
static void timeout_cancel(struct timeout *t) { (void)t; poll_fn = NULL; }
static void bg_task_post(void *task, long id) { (void)task; (void)id; posts++; }
#define strlcpy test_strlcpy
static size_t strlcpy(char *out, const char *in, size_t size)
{ size_t n = strlen(in); assert(n < size); memcpy(out, in, n + 1); return n; }
static bool tagcache_is_usable(void) { return true; }
static unsigned art_cache_track_key(const char *path) { (void)path; return 12; }
static bool aa_thumbs_exist(unsigned key) { (void)key; return generated == ART_CACHE_NUM_SIZES; }
static bool aa_format_current(void) { return true; }
struct tagcache_search { int n; };
static bool tagcache_find_index(struct tagcache_search *s, const char *p) { (void)s; (void)p; return true; }
static void tagcache_search_finish(struct tagcache_search *s) { (void)s; }
static size_t aa_work_bytes(void) { return 100; }
static size_t core_allocatable(void)
{ memory_checks++; if (supersede) { aa_offer.serial++; aa_offer.retries = 0; } return memory; }
static int core_alloc(size_t size) { (void)size; return 1; }
static void *core_get_data_pinned(int handle) { (void)handle; return NULL; }
static void core_unpin(int h) { (void)h; }
static void core_free(int h) { (void)h; }
static void aa_ensure_dirs(void) {}
static void aa_sizes_largest_first(int *order) { for (int i = 0; i < 3; i++) order[i] = i; }
static bool aa_check_abort(void) { return false; }
static void aa_cache_path(char *p, int size, int s, unsigned key) { (void)s; (void)key; strlcpy(p, "cache.aat", size); }
static bool file_exists(const char *p) { (void)p; return false; }
static void aa_generate_size(const struct aa_src *src, int s, unsigned key, const char *out, void *buf, size_t size)
{ (void)s; (void)key; (void)out; (void)buf; (void)size; assert(!strcmp(src->path, "/song.mp3")); generated++; generation++; offer_frame(); }
static void aa_stamp_append(unsigned key, unsigned stamp) { (void)key; (void)stamp; }
static void yield(void) {}
'''
# Run Cover Flow's actual refresh block between each simulated publication.
loop = function('apps-ipod/screens/covers/carousel.c', 'album_covers_loop')
start = loop.index('        unsigned art_generation = art_cache_generation();')
end = loop.index('        /* Get input first.', start)
code += r'''
#define EV_WAKEUP 1
static int thread_q;
static unsigned art_seen_generation;
static bool art_owed;
static void buf_ctx_lock(void) {}
static void buf_ctx_unlock(void) {}
static void free_all_slide_prio(int p) { (void)p; reloads++; }
static void queue_remove_from_head(void *q, int id) { (void)q; (void)id; }
static void queue_post(void *q, int id, int data) { (void)q; (void)id; (void)data; }
static void offer_frame(void)
{
''' + loop[start:end] + '\n}\n'
for name in ['aa_disk_asleep', 'aa_offer_poll', 'aa_offer_retry', 'aa_handle_offer', 'aa_track_change_cb']:
    if name == 'aa_offer_retry' and 'static void aa_offer_retry(' not in (ROOT / 'apps-ipod/metadata/art_cache.c').read_text():
        continue
    code += function('apps-ipod/metadata/art_cache.c', name)
code += r'''
static void poll(void)
{
    assert(poll_fn);
    int (*fn)(struct timeout *) = poll_fn;
    if (!fn(&aa_offer_tmo)) poll_fn = NULL;
}
int main(void)
{
    strcpy(id3.path, "/song.mp3");
    id3.has_embedded_albumart = true;
    id3.albumart.size = 100;
    asleep = true;
    aa_track_change_cb(0, NULL);
    assert(posts == 1 && !generated);
    aa_handle_offer();
    assert(poll_fn && !generated && !memory_checks);
    poll();
    assert(posts == 1);
    asleep = false;
    poll();
    assert(posts == 2);
    aa_handle_offer();
    assert(poll_fn);
    for (int i = 0; i < 3; i++) { poll(); aa_handle_offer(); }
    assert(!poll_fn && memory_checks == 4);
    memory = 1000;
    aa_io_failed = true;
    aa_track_change_cb(0, NULL);
    aa_handle_offer();
    assert(generated == 3 && !aa_io_failed);
    offer_frame();
    assert(reloads == 1 && !cache_busy);
    offer_frame();
    assert(reloads == 1);
    generated = 0;
    memory = 0;
    supersede = true;
    aa_track_change_cb(0, NULL);
    aa_handle_offer();
    assert(!poll_fn && !aa_offer.retries);
    supersede = false;
    memory = 1000;
    global_settings.art_cache_album_source = ART_SOURCE_FILES;
    aa_handle_offer();
    assert(!generated);
    puts("sleep/wake, three retries, new-offer reset, superseded offer policy recheck and one refresh per offer PASS");
    return 0;
}
'''
source = OUT / 'offers.c'
source.write_text(code)
run([CC, '-std=gnu11', '-g', '-O1', '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', source, '-o', OUT / 'offers'], 'compile-offers')
print(run([OUT / 'offers'], 'offers').stdout)
