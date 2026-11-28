/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Split Album Artists: "Madonna; Charli XCX" in Album Artist files the album
 * under both. The database holds that string as one album artist and cannot
 * hold two, so this is derived, as the guest table is: the strings holding a
 * ';' ("joint" strings), cut by db_names_split(), and the artists they name
 * ("parts"), each with the joint strings naming them and the string that is
 * their name alone, if the library has one.
 *
 * Nothing here changes what the database stores. A part is browsed by a
 * handle that tagcache's filter and tagcache_seek_string() understand, so
 * every list below an artist works unchanged.
 *
 * Fixed-size BSS, as db_featured.c's table is, for its reason: an allocation
 * from a screen shrinks the audio buffer and rebuffers the track.
 *
 * Parts, in order:
 *   - the table
 *   - building it: the joint strings, then the parts' own strings
 *   - db_names_ensure(), and reading the table
 *   - showing a joint string
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "settings/settings.h"
#include "database/tagcache.h"
#include "database/db_featured.h"   /* db_names_split(), name identity */
#include "metadata/art_name.h"
#include "logf.h"
#include "db_names.h"

/* ------------------------------------------------------------------ *
 * the table                                                          *
 * ------------------------------------------------------------------ */

struct part {
    uint32_t key;       /* db_featured_name_hash() of the name */
    uint16_t name_off;  /* into arena, NUL-terminated */
    int32_t  own_seek;  /* the album-artist string that is this name, or -1 */
};

/* A joint string: its seek, and its parts as a run of links[] */
struct joint {
    int32_t  seek;
    uint16_t first;
    uint16_t count;
};

static struct part parts[DB_NAMES_PARTS_MAX];
static int part_ct;
static struct joint joints[DB_NAMES_JOINTS_MAX];
static int joint_ct;
static uint16_t links[DB_NAMES_LINKS_MAX];
static int link_ct;
static char arena[DB_NAMES_ARENA];
static int arena_used;
/* The parts that have a string of their own, in the order of its seek */
static uint16_t own_order[DB_NAMES_PARTS_MAX];
static int own_ct;

static bool built, building;
static unsigned int builder;        /* the thread building, while 'building' */
static struct tagcache_marks built_marks;
static bool truncated;              /* a joint string stayed whole */
static long build_ticks;

static int part_find(const char *name, int len, uint32_t key)
{
    for (int i = 0; i < part_ct; i++)
    {
        const char *held = arena + parts[i].name_off;

        if (parts[i].key == key
            && db_featured_name_eq(held, (int)strlen(held), name, len))
            return i;
    }
    return -1;
}

/* The part called [name, len), added if new; -1 if it will not fit */
static int part_index(const char *name, int len)
{
    uint32_t key = db_featured_name_hash(name, len);
    int i = part_find(name, len, key);

    if (i >= 0)
        return i;
    if (part_ct == DB_NAMES_PARTS_MAX || arena_used + len + 1 > DB_NAMES_ARENA)
    {
        logf("split album artists: table full");
        truncated = true;
        return -1;
    }
    memcpy(arena + arena_used, name, len);
    arena[arena_used + len] = '\0';
    parts[part_ct].key = key;
    parts[part_ct].name_off = (uint16_t)arena_used;
    parts[part_ct].own_seek = -1;
    arena_used += len + 1;
    return part_ct++;
}

/* ------------------------------------------------------------------ *
 * building                                                           *
 * ------------------------------------------------------------------ */

/* The build reads RAM and never blocks; this hands the processor round
 * every HZ/25, as db_featured.c's does */
static void timed_yield(void)
{
    static long next;

    if (TIME_AFTER(current_tick, next))
    {
        yield();
        next = current_tick + HZ/25;
    }
}

/* One joint string into the table, whole or not at all: one the table has
 * no room for stays one artist. The names are db_names_split()'s, which has
 * already left out any it cannot keep (one byte, past 63 or past the 8th). */
static void add_joint(long seek, const struct db_featured_names *names)
{
    struct joint *j = &joints[joint_ct];
    int parts_before = part_ct, arena_before = arena_used;

    if (joint_ct == DB_NAMES_JOINTS_MAX
        || link_ct + names->count > DB_NAMES_LINKS_MAX)
    {
        logf("split album artists: table full");
        truncated = true;
        return;
    }
    j->seek = seek;
    j->first = (uint16_t)link_ct;
    j->count = 0;
    for (int i = 0; i < names->count; i++)
    {
        int p = part_index(names->name[i], names->len[i]);
        int k;

        if (p < 0)
        {
            link_ct = j->first;
            part_ct = parts_before;
            arena_used = arena_before;
            return;
        }
        for (k = j->first; k < link_ct && links[k] != p; k++)
            ;
        if (k < link_ct)
            continue;               /* "A; A" names A once */
        links[link_ct++] = (uint16_t)p;
        j->count++;
    }
    joint_ct++;
}

static int compare_joints(const void *a, const void *b)
{
    long sa = ((const struct joint *)a)->seek;
    long sb = ((const struct joint *)b)->seek;

    return sa < sb ? -1 : sa > sb;
}

static int compare_own(const void *a, const void *b)
{
    long sa = parts[*(const uint16_t *)a].own_seek;
    long sb = parts[*(const uint16_t *)b].own_seek;

    return sa < sb ? -1 : sa > sb;
}

/* Every album-artist string, twice: the joint ones first, since a part's own
 * string is known as one only once the parts are */
static void build(void)
{
    static struct tagcache_search tcs;
    static char buf[TAGCACHE_BUFSZ];
    static struct db_featured_names names;

    part_ct = joint_ct = link_ct = arena_used = own_ct = 0;
    truncated = false;

    if (!tagcache_search(&tcs, tag_albumartist))
        return;
    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
    {
        if (db_names_split(buf, &names) > 0)
            add_joint(tcs.result_seek, &names);
        timed_yield();
    }
    tagcache_search_finish(&tcs);
    qsort(joints, joint_ct, sizeof(joints[0]), compare_joints);

    if (part_ct == 0 || !tagcache_search(&tcs, tag_albumartist))
        return;
    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
    {
        int len = (int)strlen(buf);
        int p;

        if (memchr(buf, ';', len) == NULL
            && (p = part_find(buf, len, db_featured_name_hash(buf, len))) >= 0
            && parts[p].own_seek < 0)
        {
            parts[p].own_seek = tcs.result_seek;
            own_order[own_ct++] = (uint16_t)p;
        }
        timed_yield();
    }
    tagcache_search_finish(&tcs);
    qsort(own_order, own_ct, sizeof(own_order[0]), compare_own);
}

/* ------------------------------------------------------------------ *
 * the table, kept current and read                                   *
 * ------------------------------------------------------------------ */

static void discard(void)
{
    part_ct = joint_ct = link_ct = arena_used = own_ct = 0;
    built = false;
}

bool db_names_ensure(void)
{
    struct tagcache_marks now;

    if (!global_settings.split_album_artists)
    {
        discard();
        return false;
    }
    /* The RAM copy is out for a moment around a commit or USB; the table
     * waits for it, and db_names_active() says no meanwhile */
    if (!tagcache_is_in_ram())
        return db_names_active();
    db_names_wait();

    tagcache_get_marks(&now);
    if (!built || now.commitid != built_marks.commitid
        || now.generation != built_marks.generation)
    {
        long tick = current_tick;

        building = true;
        builder = thread_self();
        cpu_boost(true);
        build();
        cpu_boost(false);
        building = false;
        built = true;
        built_marks = now;
        build_ticks = current_tick - tick;
    }
    return db_names_active();
}

void db_names_wait(void)
{
    while (building && builder != thread_self())
        sleep(1);
}

void db_names_get_stats(struct db_names_stats *out)
{
    out->parts = part_ct;
    out->joints = joint_ct;
    out->links = link_ct;
    out->arena_used = arena_used;
    out->build_ticks = build_ticks;
    out->truncated = truncated;
}

bool db_names_active(void)
{
    struct tagcache_marks now;

    if (!global_settings.split_album_artists || !built || building
        || part_ct == 0 || !tagcache_is_in_ram())
        return false;
    tagcache_get_marks(&now);
    return now.commitid == built_marks.commitid
           && now.generation == built_marks.generation;
}

/* The joint string at 'seek', by binary search, or NULL */
static const struct joint *joint_at(long seek)
{
    int lo = 0, hi = joint_ct - 1;

    if (building)
        return NULL;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (joints[mid].seek == seek)
            return &joints[mid];
        if (joints[mid].seek < seek)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

bool db_names_is_joint(long seek)
{
    return joint_at(seek) != NULL;
}

int db_names_count(void)
{
    return building ? 0 : part_ct;
}

/* Part n is there to read: a build in another thread empties the table, and
 * a caller's n is from before the yield that let it start */
static bool part_ok(int n)
{
    return !building && n >= 0 && n < part_ct;
}

const char *db_names_name(int n)
{
    return part_ok(n) ? arena + parts[n].name_off : "";
}

long db_names_handle(int n)
{
    return DB_NAMES_HANDLE_FIRST - n;
}

long db_names_own_seek(int n)
{
    return part_ok(n) ? parts[n].own_seek : -1;
}

int db_names_index(long handle)
{
    long n = DB_NAMES_HANDLE_FIRST - handle;

    return db_names_is_handle(handle) && !building && n < part_ct
           ? (int)n : -1;
}

int db_names_find(const char *name)
{
    int len = (int)strlen(name);

    return building ? -1
                    : part_find(name, len, db_featured_name_hash(name, len));
}

int db_names_joint_parts(long seek, int *out, int max)
{
    const struct joint *j = joint_at(seek);
    int n = 0;

    for (; j && n < j->count && n < max; n++)
        out[n] = links[j->first + n];
    return n;
}

bool db_names_match(long handle, long seek)
{
    int p = db_names_index(handle);
    const struct joint *j;

    if (p < 0)
        return false;
    if (seek == parts[p].own_seek)
        return true;
    if ((j = joint_at(seek)) == NULL)
        return false;
    for (int k = 0; k < j->count; k++)
        if (links[j->first + k] == p)
            return true;
    return false;
}

long db_names_browse_seek(long seek)
{
    int lo = 0, hi = own_ct - 1;

    if (seek < 0 || !db_names_active())
        return seek;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        long own = parts[own_order[mid]].own_seek;

        if (own == seek)
            return db_names_handle(own_order[mid]);
        if (own < seek)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return seek;
}

int db_names_strings(int n, long *out, int max)
{
    int ct = 0;

    if (!part_ok(n))
        return 0;
    if (parts[n].own_seek >= 0 && ct < max)
        out[ct++] = parts[n].own_seek;
    for (int j = 0; j < joint_ct && ct < max; j++)
        for (int k = 0; k < joints[j].count; k++)
            if (links[joints[j].first + k] == n)
            {
                out[ct++] = joints[j].seek;
                break;
            }
    return ct;
}

bool db_names_artist(int n, bool (*skip)(const struct tagcache_artist *),
                     struct tagcache_artist *out)
{
    static long strings[DB_NAMES_JOINTS_MAX + 1];   /* off the stack */
    int ct = db_names_strings(n, strings, ARRAYLEN(strings));
    bool any = false;

    memset(out, 0, sizeof(*out));
    out->seek = db_names_handle(n);
    out->art_hash = art_name_hash(db_names_name(n));
    for (int i = 0; i < ct; i++)
    {
        struct tagcache_artist r;

        if (!tagcache_artist_get(tagcache_artist_find(strings[i]), &r)
            || (skip && skip(&r)))
            continue;
        any = true;
        out->playcount += r.playcount;
        if (r.lastplayed > out->lastplayed)
            out->lastplayed = r.lastplayed;
        out->albums += r.albums;
        out->spoken_albums += r.spoken_albums;
        out->podcast_albums += r.podcast_albums;
        if (r.cover_hash && (!out->cover_hash || r.cover_year > out->cover_year))
        {
            out->cover_hash = r.cover_hash;
            out->cover_year = r.cover_year;
        }
    }
    return any;
}

/* ------------------------------------------------------------------ *
 * showing a joint string                                             *
 * ------------------------------------------------------------------ */

const char *db_names_display(const char *raw, char *buf, size_t size)
{
    struct db_featured_names names;
    const char *s;
    size_t used = 0;
    int semis = 0;

    if (!raw || buf == raw || size == 0
        || !global_settings.split_album_artists
        || db_names_split(raw, &names) < 2)
        return raw;
    for (s = raw; (s = strchr(s, ';')); s++)
        semis++;
    if (names.count != semis + 1)
        return raw;             /* a name dropped: shown as tagged */
    for (int i = 0; i < names.count; i++)
    {
        const char *sep = i == 0 ? "" : i == names.count - 1 ? " & " : ", ";
        int n = snprintf(buf + used, size - used, "%s%.*s", sep,
                         names.len[i], names.name[i]);

        if (n < 0 || (size_t)n >= size - used)
            return raw;
        used += n;
    }
    return buf;
}
