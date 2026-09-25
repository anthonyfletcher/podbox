/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Music Quiz's tracks: ten to play, and four wrong titles beside each.
 *
 * The game gets harder as it goes, and harder means the wrong titles are more
 * like the right one. Where the Sound Index has measured the track that
 * plays, "like" is how it sounds: the first round draws from the forty-odd
 * tracks nearest it and the last from the nearest handful. The distance is
 * Play Similar's (database/sound_mix.c) with its genre and era terms switched
 * off -- they make a mix hang together, and here they would make the right
 * answer the odd one out. Where the index has not, "like" is the same genre
 * and the same decade, read out of the database, and the rounds climb from
 * anything at all to both.
 *
 * Everything runs in the app buffer and is copied into the caller's rounds
 * before it is let go, because the quiz then builds a playlist, and building
 * one borrows the same buffer.
 *
 * Parts, in order:
 *   - the arena
 *   - a random sample of the library
 *   - the rounds, and wrong titles by genre and decade
 *   - wrong titles by sound
 *   - the way in
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "cpu.h"                      /* cpu_boost */
#include "system/app_buffer.h"
#include "database/tagcache.h"
#include "database/sound_index.h"
#include "database/sound_mix.h"
#include "games/quiz/quiz_pick.h"

/* How many tracks the library is sampled for: ten to play and the rest to
 * draw wrong titles from by genre and decade. Enough that a genre with a
 * twentieth of the library still offers a dozen. */
#define POOL_MAX        256

/* Shorter than this and a clip from partway in would run into the end. */
#define QUIZ_MIN_LENGTH_MS  90000

/* How many of each track's nearest the index pass keeps, and how far down
 * that list each round starts: round 1 at RANK_STEP * 9, the last at the
 * top. */
#define NEAR_MAX        60
#define RANK_STEP       5

/* How alike a wrong title has to be to the right one in each round, where
 * the index cannot say: 1 is the same decade, 2 the same genre, 3 both. A
 * round that cannot find enough takes the next closest. */
static const int like_wanted[QUIZ_ROUNDS] = { 0, 0, 0, 1, 1, 1, 2, 2, 3, 3 };

/* sound_mix.c's MIX_SEED_EPSILON: closer than this, with the same length, two
 * records are one recording encoded twice, and its own title under another
 * spelling would be a wrong answer that is right. */
#define SAME_RECORDING_D    15
#define SAME_LENGTH_MS      2000

#define ALBUM_MAX       64
#define GENRE_MAX       32

/* ------------------------------------------------------------------ *
 * the arena                                                          *
 * ------------------------------------------------------------------ */

struct pool_track
{
    int32_t  idx_id;
    long     length;
    int      year;
    char     title[QUIZ_TITLE_MAX];
    char     album[ALBUM_MAX];
    char     genre[GENRE_MAX];
    bool     answer;            /* one of the ten, so never a wrong title */
};

struct near
{
    uint64_t key;
    int      d;
};

struct sound_answer
{
    int round;
    uint64_t key;
    long length;
    struct sound_axes ax;
    struct near near[NEAR_MAX];
    int near_ct;
};

/* A neighbour's key, and what the database says is behind it. */
struct resolved
{
    uint64_t key;
    long     length;
    bool     found;
    char     title[QUIZ_TITLE_MAX];
    char     album[ALBUM_MAX];
};

static struct pool_track   *pool;
static struct sound_answer *answers;
static struct resolved     *res;
static int pool_ct, answer_ct, res_ct, res_cap;

/* Which pool_track each round's answer is. */
static int answer_pool[QUIZ_ROUNDS];

static bool claim(void)
{
    size_t sz, fixed;
    char *p = app_claim_buffer(&sz, "music quiz");

    fixed = POOL_MAX * sizeof(*pool) + QUIZ_ROUNDS * sizeof(*answers);
    if (p == NULL || sz < fixed + NEAR_MAX * sizeof(*res))
    {
        if (p != NULL)
            app_release_buffer("music quiz");
        return false;
    }

    pool    = (struct pool_track *)p;    p += POOL_MAX * sizeof(*pool);
    answers = (struct sound_answer *)p;  p += QUIZ_ROUNDS * sizeof(*answers);
    res     = (struct resolved *)p;
    res_cap = (int)((sz - fixed) / sizeof(*res));

    pool_ct = answer_ct = res_ct = 0;
    return true;
}

static void release(void)
{
    app_release_buffer("music quiz");
}

/* ------------------------------------------------------------------ *
 * a random sample of the library                                     *
 * ------------------------------------------------------------------ */

/* Music only. tagcache keeps the pointer rather than a copy. */
static struct tagcache_search_clause music_clause = {
    .tag = tag_virt_spoken,
    .type = clause_is,
    .numeric = true,
    .source = source_constant,
    .numeric_data = 0,
    .str = NULL,
};

static bool text_usable(const char *s)
{
    return s[0] != '\0' && strcmp(s, UNTAGGED) != 0;
}

static bool title_taken(const struct quiz_round *r, int ct, const char *title)
{
    for (int i = 0; i < ct; i++)
    {
        if (!strcasecmp(r->title[i], title))
            return true;
    }
    return false;
}

/* The answers: the first ten of the sample with titles unlike each other's,
 * the sample being in random order already. Their paths are read here, while
 * the search that can retrieve them is still open. */
static int choose_answers(struct tagcache_search *tcs,
                          struct quiz_round *rounds)
{
    int n = 0;

    for (int i = 0; i < pool_ct && n < QUIZ_ROUNDS; i++)
    {
        bool clash = false;

        for (int j = 0; j < n && !clash; j++)
            clash = !strcasecmp(rounds[j].title[0], pool[i].title);
        if (clash)
            continue;

        memset(&rounds[n], 0, sizeof(rounds[n]));
        if (!tagcache_retrieve(tcs, pool[i].idx_id, tag_filename,
                               rounds[n].path, sizeof(rounds[n].path)))
            continue;

        pool[i].answer = true;
        strmemccpy(rounds[n].title[0], pool[i].title, QUIZ_TITLE_MAX);
        rounds[n].length = (unsigned long)pool[i].length;
        answer_pool[n] = i;
        n++;
    }

    return n;
}

/* POOL_MAX tracks drawn evenly from the whole library, in one walk: each
 * track met replaces a random slot with falling odds, which leaves every
 * track equally likely to be held at the end. Then the ten answers. */
static int sample_library(struct quiz_round *rounds)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    long seen = 0;
    int i, answers_found;

    if (!tagcache_search(&tcs, tag_filename))
        return -1;
    tagcache_search_add_clause(&tcs, &music_clause);

    while (tagcache_get_next(&tcs, path, sizeof(path)))
    {
        long length = tagcache_get_numeric(&tcs, tag_length);
        long slot;

        if (length < QUIZ_MIN_LENGTH_MS)
            continue;

        /* Numbers are read now: tagcache_get_numeric() answers only for the
         * entry the walk is on. Strings can be fetched later by index. */
        slot = seen < POOL_MAX ? seen : rand() % (seen + 1);
        if (slot < POOL_MAX)
        {
            pool[slot].idx_id = tcs.idx_id;
            pool[slot].length = length;
            pool[slot].year = (int)tagcache_get_numeric(&tcs, tag_year);
        }
        seen++;

        if ((seen & 63) == 0)
            yield();
    }

    pool_ct = 0;
    for (i = 0; i < (seen < POOL_MAX ? seen : POOL_MAX); i++)
    {
        struct pool_track t = pool[i];

        t.answer = false;
        if (!tagcache_retrieve(&tcs, t.idx_id, tag_title, t.title,
                               sizeof(t.title))
            || !text_usable(t.title))
            continue;

        tagcache_retrieve(&tcs, t.idx_id, tag_album, t.album, sizeof(t.album));
        if (!text_usable(t.album))
            t.album[0] = '\0';
        tagcache_retrieve(&tcs, t.idx_id, tag_genre, t.genre, sizeof(t.genre));
        if (!text_usable(t.genre))
            t.genre[0] = '\0';

        pool[pool_ct++] = t;
    }

    /* Shuffled before the answers are taken from the front. The sample is
     * the right set but not in a random order: a library smaller than it is
     * held whole in database order, and would open every game on the same
     * ten tracks. */
    for (i = pool_ct - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        struct pool_track t = pool[i];

        pool[i] = pool[j];
        pool[j] = t;
    }

    answers_found = choose_answers(&tcs, rounds);
    tagcache_search_finish(&tcs);
    return answers_found;
}

/* ------------------------------------------------------------------ *
 * the rounds, and wrong titles by genre and decade                   *
 * ------------------------------------------------------------------ */

/* How alike two tracks are by what the database knows: 2 for the genre, 1
 * for the decade, summed. */
static int likeness(const struct pool_track *a, const struct pool_track *b)
{
    int like = 0;

    if (a->genre[0] && !strcasecmp(a->genre, b->genre))
        like += 2;
    if (a->year > 0 && b->year > 0 && a->year / 10 == b->year / 10)
        like += 1;
    return like;
}

/* Wrong titles from the sample until the round has all its choices, the
 * alike ones first. 'ct' is how many it has already. */
static int add_like_titles(struct quiz_round *r, int round, int ct)
{
    const struct pool_track *a = &pool[answer_pool[round]];
    int start = rand() % pool_ct;

    for (int want = like_wanted[round]; want >= 0 && ct < QUIZ_CHOICES; want--)
    {
        for (int k = 0; k < pool_ct && ct < QUIZ_CHOICES; k++)
        {
            const struct pool_track *t = &pool[(start + k) % pool_ct];

            if (t->answer || likeness(a, t) < want
                || title_taken(r, ct, t->title))
                continue;
            strmemccpy(r->title[ct++], t->title, QUIZ_TITLE_MAX);
        }
    }

    return ct;
}

/* The answer is in title[0] while the wrong ones are gathered; this moves it
 * to a random place among them. */
static void shuffle_answer(struct quiz_round *r)
{
    char held[QUIZ_TITLE_MAX];
    int at = rand() % QUIZ_CHOICES;

    r->right = at;
    if (at == 0)
        return;

    memcpy(held, r->title[at], sizeof(held));
    memcpy(r->title[at], r->title[0], sizeof(held));
    memcpy(r->title[0], held, sizeof(held));
}

/* ------------------------------------------------------------------ *
 * wrong titles by sound                                              *
 * ------------------------------------------------------------------ */

static void keep_near(struct sound_answer *a, uint64_t key, int d)
{
    int i;

    if (a->near_ct == NEAR_MAX && d >= a->near[NEAR_MAX - 1].d)
        return;

    i = a->near_ct < NEAR_MAX ? a->near_ct++ : NEAR_MAX - 1;
    while (i > 0 && a->near[i - 1].d > d)
    {
        a->near[i] = a->near[i - 1];
        i--;
    }
    a->near[i].key = key;
    a->near[i].d = d;
}

/* The rounds whose track the index has measured, and each one's nearest.
 * False only where there is no index to read. */
static bool index_pass(const struct quiz_round *rounds)
{
    struct sound_index_reader r;
    struct sound_record rec;

    if (sound_index_reader_open(&r) != SOUND_OK)
        return false;

    answer_ct = 0;
    for (int i = 0; i < QUIZ_ROUNDS; i++)
    {
        struct sound_answer *a = &answers[answer_ct];
        uint64_t key = sound_index_key(rounds[i].path);

        if (!sound_index_find(&r, key, &rec) || !sound_record_usable(&rec))
            continue;

        a->round = i;
        a->key = key;
        a->length = (long)rounds[i].length;
        a->near_ct = 0;
        sound_mix_axes(&rec, &a->ax);
        /* Sound alone -- see the head of the file. Both terms are skipped
         * at zero. */
        a->ax.genre = 0;
        a->ax.year = 0;
        answer_ct++;
    }

    for (int n = 0; answer_ct > 0 && n < r.count; n++)
    {
        struct sound_axes ax;

        if ((n & 63) == 0)
            yield();
        if (!sound_index_read(&r, n, &rec) || !sound_record_usable(&rec))
            continue;

        sound_mix_axes(&rec, &ax);
        ax.genre = 0;
        ax.year = 0;

        for (int i = 0; i < answer_ct; i++)
        {
            if (rec.key != answers[i].key)
                keep_near(&answers[i], rec.key,
                          sound_mix_distance(&answers[i].ax, &ax));
        }
    }

    sound_index_reader_close(&r);
    return true;
}

static int compare_res(const void *a_v, const void *b_v)
{
    const struct resolved *a = a_v;
    const struct resolved *b = b_v;

    return a->key < b->key ? -1 : a->key > b->key;
}

static struct resolved *find_res(uint64_t key)
{
    int lo = 0, hi = res_ct - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;

        if (res[mid].key == key)
            return &res[mid];
        if (res[mid].key < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* An index record carries no path, so one walk of the database is the only
 * way back from the neighbours' keys to their titles. */
static bool resolve_near(void)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    int n = 0;

    res_ct = 0;
    for (int i = 0; i < answer_ct; i++)
    {
        for (int k = 0; k < answers[i].near_ct && res_ct < res_cap; k++)
        {
            memset(&res[res_ct], 0, sizeof(res[0]));
            res[res_ct++].key = answers[i].near[k].key;
        }
    }
    qsort(res, res_ct, sizeof(*res), compare_res);

    /* Two answers can share a neighbour, and find_res() must see it once. */
    if (res_ct > 1)
    {
        int out = 1;

        for (int i = 1; i < res_ct; i++)
        {
            if (res[i].key != res[out - 1].key)
                res[out++] = res[i];
        }
        res_ct = out;
    }

    if (res_ct == 0)
        return true;
    if (!tagcache_search(&tcs, tag_filename))
        return false;
    tagcache_search_add_clause(&tcs, &music_clause);

    while (tagcache_get_next(&tcs, path, sizeof(path)))
    {
        struct resolved *e;

        if ((++n & 15) == 0)
            yield();

        e = find_res(sound_index_key(path));
        if (e == NULL || e->found)
            continue;

        e->length = tagcache_get_numeric(&tcs, tag_length);
        if (!tagcache_retrieve(&tcs, tcs.idx_id, tag_title, e->title,
                               sizeof(e->title))
            || !text_usable(e->title))
            continue;
        tagcache_retrieve(&tcs, tcs.idx_id, tag_album, e->album,
                          sizeof(e->album));
        if (!text_usable(e->album))
            e->album[0] = '\0';
        e->found = true;
    }

    tagcache_search_finish(&tcs);
    return true;
}

static bool album_taken(char taken[][ALBUM_MAX], int ct, const char *album)
{
    if (album[0] == '\0')
        return false;
    for (int i = 0; i < ct; i++)
    {
        if (!strcasecmp(taken[i], album))
            return true;
    }
    return false;
}

static bool near_usable(const struct sound_answer *a, int k,
                        const struct quiz_round *r, int ct,
                        char albums[][ALBUM_MAX], int albums_ct)
{
    const struct resolved *e = find_res(a->near[k].key);
    long gap;

    if (e == NULL || !e->found || e->length < QUIZ_MIN_LENGTH_MS)
        return false;
    if (title_taken(r, ct, e->title) || album_taken(albums, albums_ct, e->album))
        return false;

    gap = e->length > a->length ? e->length - a->length
                                : a->length - e->length;
    return !(a->near[k].d < SAME_RECORDING_D && gap < SAME_LENGTH_MS);
}

/* Up to four wrong titles from the answer's neighbours: from this round's
 * place down the list, and then back up towards the nearest for what that
 * left unfilled. At most one from any album -- the nearest are mostly the
 * answer's own album otherwise. */
static int add_near_titles(struct quiz_round *r, const struct sound_answer *a)
{
    char albums[QUIZ_CHOICES][ALBUM_MAX];
    int from = RANK_STEP * (QUIZ_ROUNDS - 1 - a->round);
    int ct = 1, albums_ct = 0;

    if (from > a->near_ct)
        from = a->near_ct;

    for (int k = from; k < a->near_ct && ct < QUIZ_CHOICES; k++)
    {
        const struct resolved *e;

        if (!near_usable(a, k, r, ct, albums, albums_ct))
            continue;
        e = find_res(a->near[k].key);
        strmemccpy(r->title[ct++], e->title, QUIZ_TITLE_MAX);
        if (e->album[0])
            strmemccpy(albums[albums_ct++], e->album, ALBUM_MAX);
    }

    for (int k = from - 1; k >= 0 && ct < QUIZ_CHOICES; k--)
    {
        const struct resolved *e;

        if (!near_usable(a, k, r, ct, albums, albums_ct))
            continue;
        e = find_res(a->near[k].key);
        strmemccpy(r->title[ct++], e->title, QUIZ_TITLE_MAX);
        if (e->album[0])
            strmemccpy(albums[albums_ct++], e->album, ALBUM_MAX);
    }

    return ct;
}

/* ------------------------------------------------------------------ *
 * the way in                                                         *
 * ------------------------------------------------------------------ */

int quiz_pick(struct quiz_round *rounds)
{
    int have[QUIZ_ROUNDS];
    int ret = QUIZ_PICK_OK;
    int found;

    if (!claim())
        return QUIZ_PICK_NO_MEM;

    srand(current_tick);
    cpu_boost(true);

    found = sample_library(rounds);
    if (found < 0)
    {
        ret = QUIZ_PICK_NO_DB;
        goto out;
    }
    if (found < QUIZ_ROUNDS)
    {
        ret = QUIZ_PICK_TOO_FEW;
        goto out;
    }

    for (int i = 0; i < QUIZ_ROUNDS; i++)
        have[i] = 1;

    /* No index is not a failure: every round then goes by genre and decade. */
    if (sound_index_exists() && index_pass(rounds) && resolve_near())
    {
        for (int i = 0; i < answer_ct; i++)
            have[answers[i].round] = add_near_titles(&rounds[answers[i].round],
                                                     &answers[i]);
    }

    for (int i = 0; i < QUIZ_ROUNDS; i++)
    {
        struct quiz_round *r = &rounds[i];

        if (add_like_titles(r, i, have[i]) < QUIZ_CHOICES)
        {
            ret = QUIZ_PICK_TOO_FEW;
            goto out;
        }
        shuffle_answer(r);
    }

out:
    cpu_boost(false);
    release();
    return ret;
}
