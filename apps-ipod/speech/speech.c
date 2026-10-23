/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The synthesiser as the player uses it: what it loads, the text it is
 * given, and the samples it renders for the voice thread.
 *
 * Two files are loaded while Voice Menus and Speech Synthesiser are both on:
 * the dictionary, .rockbox/speech/cmudict.bin, for lexicon.c, and the
 * language's voice strings, .rockbox/langs/<language>.vstrings (English's
 * when the language has none), the text a .voice file would have recorded
 * for each language string. Both are
 * loaded at boot by talk_init(), before the audio buffer takes the memory.
 *
 * Text is turned into phonemes on the caller's thread, when it is queued,
 * so that the voice thread only renders. The phonemes wait in a ring, one
 * item per talk.c queue entry, each item a run of clauses:
 *     u16 units that follow, then per clause: u16 clause, u16 length, IPA
 * Items are released in the order they were queued, so the ring frees from
 * its tail.
 *
 * Parts, in order:
 *   - loading: the dictionary and the voice strings
 *   - the phoneme ring
 *   - queueing text
 *   - rendering, on the voice thread
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "string-extra.h"
#include "file.h"
#include "core_alloc.h"
#include "rbpaths.h"
#include "lang.h"
#include "settings/settings.h"
#include "lexicon.h"
#include "normalise.h"
#include "synth.h"
#include "synth_frames.h"
#include "talk.h"
#include "speech.h"

#define DICT_FILE  ROCKBOX_DIR "/speech/cmudict.bin"
#define DEFAULT_LANG "english"

#define RING_SIZE  8192         /* phoneme units, for every queued item: a
                                 * text viewer page of 4 KB fits */
#define CHUNK_IPA  160          /* units per clause handed to the prosody */
#define BASE_SPEED 750          /* thousandths of NV Speech Player's rate */

#define VSTRINGS_COOKIE  0x9a
#define VSTRINGS_VERSION 0x06
#define VSTRINGS_HEADER  10
#define ID1_COUNT TALK_FINAL_ID
#define ID2_COUNT (TALK_FINAL_ID_VOICEONLY - VOICEONLY_DELIMITER - 1)
#define NO_STRING 0xffff

/** Loading **/

static int dict_handle, strings_handle;
static size_t dict_size;
static char strings_lang[MAX_FILENAME + 1];

static int dict_move(int handle, void *current, void *new)
{
    (void)handle;
    (void)current;
    lexicon_init(new, dict_size);
    return BUFLIB_CB_OK;
}

static struct buflib_callbacks dict_ops = { .move_callback = dict_move };

/* Read a whole file into a new allocation, extra bytes after the file left
 * for the caller. Returns the handle, or 0. */
static int load_file(const char *path, size_t extra,
                     struct buflib_callbacks *ops, size_t *size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    off_t len = ffilesize(fd);
    int handle = len > 0 ? core_alloc_ex(len + extra, ops) : -1;
    if (handle > 0)
    {
        core_pin(handle);
        if (read(fd, core_get_data(handle), len) != len)
            handle = core_free(handle);
        else
            core_unpin(handle);
    }
    close(fd);
    *size = len;
    return handle > 0 ? handle : 0;
}

static void load_dictionary(void)
{
    dict_handle = load_file(DICT_FILE, 0, &dict_ops, &dict_size);
    if (dict_handle && !lexicon_init(core_get_data(dict_handle), dict_size))
        dict_handle = core_free(dict_handle);
}

static int id_slot(int32_t id)
{
    if (id >= 0 && id < ID1_COUNT)
        return id;
    if (id > VOICEONLY_DELIMITER && id < TALK_FINAL_ID_VOICEONLY)
        return ID1_COUNT + id - VOICEONLY_DELIMITER - 1;
    return -1;
}

static void load_strings(const char *lang)
{
    char path[MAX_PATH];
    size_t index = (ID1_COUNT + ID2_COUNT) * sizeof(uint16_t);
    size_t size;
    snprintf(path, sizeof(path), LANG_DIR "/%s.vstrings", lang);
    strmemccpy(strings_lang, lang, sizeof(strings_lang));
    strings_handle = load_file(path, index, NULL, &size);
    if (!strings_handle)
        return;

    /* the file is read to the start; move it after the index */
    uint8_t *base = core_get_data(strings_handle);
    memmove(base + index, base, size);
    uint8_t *file = base + index;
    uint16_t *offsets = (uint16_t *)base;
    memset(offsets, 0xff, index);
    if (size < VSTRINGS_HEADER || size >= NO_STRING
        || file[0] != VSTRINGS_COOKIE || file[1] != VSTRINGS_VERSION
        || file[2] != TARGET_ID
        /* the string count, as a .voice file is checked: another build's
         * strings are numbered differently */
        || ((file[4] << 8) | file[5]) != ID1_COUNT + ID2_COUNT)
    {
        strings_handle = core_free(strings_handle);
        return;
    }
    for (size_t p = VSTRINGS_HEADER; p + 2 < size; )
    {
        int slot = id_slot((file[p] << 8) | file[p + 1]);
        p += 2;
        if (slot >= 0)
            offsets[slot] = p;
        while (p < size && file[p])
            p++;
        p++;
    }
    /* the last string ends inside the file */
    if (file[size - 1] != '\0')
        strings_handle = core_free(strings_handle);
}

void speech_apply_settings(void)
{
    const char *lang = DEFAULT_LANG;
    if (global_settings.lang_file[0] && global_settings.lang_file[0] != 0xff)
        lang = (const char *)global_settings.lang_file;

    if (!global_settings.talk_menu || !global_settings.talk_synth)
    {
        lexicon_init(NULL, 0);
        dict_handle = core_free(dict_handle);
        strings_handle = core_free(strings_handle);
        return;
    }
    if (!dict_handle)
        load_dictionary();
    if (strings_handle && strcasecmp(strings_lang, lang))
        strings_handle = core_free(strings_handle);
    if (!strings_handle)
    {
        /* the synthesiser reads English, so a language without its own
         * strings is spoken in English rather than not at all */
        load_strings(lang);
        if (!strings_handle && strcasecmp(lang, DEFAULT_LANG))
        {
            load_strings(DEFAULT_LANG);
            strmemccpy(strings_lang, lang, sizeof(strings_lang));
        }
    }
}

bool speech_enabled(void)
{
    /* without the dictionary the letter-to-sound rules read every word */
    return global_settings.talk_menu && global_settings.talk_synth
           && strings_handle;
}

bool speech_voice_string(int32_t id, char *buf, int size)
{
    int slot = id_slot(id);
    if (!strings_handle || slot < 0)
        return false;
    /* the strings' allocation: an offset into the file for each id, those
     * of the first table then the voice-only ones, followed by the file */
    const uint16_t *offsets = core_get_data(strings_handle);
    uint16_t offset = offsets[slot];
    if (offset == NO_STRING)
        return false;
    const char *s = (const char *)(offsets + ID1_COUNT + ID2_COUNT) + offset;
    strmemccpy(buf, s, size);
    return buf[0] != '\0';
}

/** The phoneme ring **/

static uint16_t ring[RING_SIZE];
static int ring_read, ring_write;   /* oldest item; where the next goes */
static int live;                    /* items not yet released */
static int newest = -1;             /* the item an append may extend */
static int newest_clause;           /* its last clause */
static int begun = -1;              /* the item being rendered */

static int item_end(int item)
{
    return item + 1 + ring[item];
}

/* Room for n units at ring_write without reaching an unreleased item. */
static bool ring_fits(int n)
{
    if (live == 0 || ring_write >= ring_read)
        return ring_write + n <= RING_SIZE;
    return ring_write + n < ring_read;
}

/* Where a new item of at least n units can start, or -1. */
static int ring_start(int n)
{
    if (live == 0)
        ring_read = ring_write = 0;
    if (ring_fits(n))
        return ring_write;
    if (ring_write >= ring_read && n < ring_read)
        return 0;
    return -1;
}

void speech_release(int item)
{
    if (live == 0)
        return;
    ring_read = item_end(item);
    if (item == newest)
        newest = -1;
    if (item == begun)
        begun = -1;
    if (--live == 0)
        ring_read = ring_write = 0;
}

void speech_clear(void)
{
    live = 0;
    ring_read = ring_write = 0;
    newest = begun = -1;
}

/** Queueing text **/

/* Append one clause chunk of words to the newest item: joined to its last
 * clause while that has no closing punctuation and room, otherwise as a
 * clause of its own. Returns how many bytes of words were used, 0 if there
 * was no room. */
static size_t put_chunk(const char *words, int clause)
{
    uint16_t *rec = &ring[newest_clause];
    int len;
    size_t used;
    if (newest_clause > newest && rec[0] == 0
        && rec[1] + 1 + CHUNK_IPA / 2 <= CHUNK_IPA)
    {
        int room = CHUNK_IPA - rec[1] - 1;
        if (!ring_fits(1 + room))
            return 0;
        ring[ring_write] = ' ';
        used = lexicon_ipa(words, &ring[ring_write + 1], room, &len);
        if (len == 0)
            return used;
        rec[1] += 1 + len;
        ring_write += 1 + len;
        ring[newest] += 1 + len;
    }
    else
    {
        if (!ring_fits(2 + CHUNK_IPA))
            return 0;
        uint16_t *r = &ring[ring_write];
        used = lexicon_ipa(words, r + 2, CHUNK_IPA, &len);
        if (len == 0)
            return used;
        r[0] = 0;
        r[1] = len;
        newest_clause = ring_write;
        ring_write += 2 + len;
        ring[newest] += 2 + len;
    }
    ring[newest_clause] = words[used] ? 0 : clause;
    return used;
}

int speech_queue(const char *utf8, int append_to)
{
    char text[3], words[256];
    int clause;

    if (!speech_enabled())
        return -1;
    if (append_to < 0 || append_to != newest || append_to == begun)
    {
        int start = ring_start(1 + 2 + CHUNK_IPA);
        if (start < 0)
            return -1;
        newest = newest_clause = ring_write = start;
        ring[newest] = 0;
        ring_write++;
        live++;
    }

    /* a letter on its own is said by its name, as an initial is */
    if (((utf8[0] | 0x20) >= 'a' && (utf8[0] | 0x20) <= 'z') && !utf8[1])
    {
        text[0] = utf8[0];
        text[1] = '.';
        text[2] = '\0';
        utf8 = text;
    }

    normalise_begin(utf8);
    while (normalise_next(words, sizeof(words), &clause))
    {
        const char *w = words;
        while (*w)
        {
            size_t used = put_chunk(w, clause);
            if (used == 0)
                break;
            w += used;
        }
    }

    if (ring[newest] == 0)
    {
        /* nothing came of it: a new item goes again, an old one stays */
        if (newest != append_to)
        {
            ring_write = newest;
            newest = -1;
            live--;
        }
        return -1;
    }
    return newest;
}

/** Rendering **/

static int render_pos, render_end;
static bool clause_open;    /* begun but not yet all queued on synth.c */

int speech_rendering(void)
{
    return begun;
}

void speech_render_begin(int item)
{
    begun = item;
    render_pos = item + 1;
    render_end = item_end(item);
    clause_open = false;
    synth_init(SPEECH_RATE);
}

int speech_render(int16_t *buf, int count)
{
    int done = 0;
    while (done < count)
    {
        if (!clause_open && render_pos < render_end)
        {
            const uint16_t *rec = &ring[render_pos];
            render_pos += 2 + rec[1];
            synth_clause_begin(rec + 2, rec[1], rec[0],
                               BASE_SPEED * global_settings.talk_speech_rate
                               / 100);
            clause_open = true;
        }
        if (clause_open && synth_clause_feed())
            clause_open = false;
        int n = synth_generate(buf + done, count - done);
        done += n;
        if (n == 0 && !clause_open && render_pos >= render_end)
            break;
    }
    return done;
}
