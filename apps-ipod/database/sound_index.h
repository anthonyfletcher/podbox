/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to sound_index.c: the per-track measurements on disk.
 ****************************************************************************/

#ifndef _SOUND_INDEX_H
#define _SOUND_INDEX_H

#include <stdbool.h>
#include <stdint.h>
#include "audio/beat_probe.h"

/* One track, as it sits in the file. Written verbatim, so every field is
 * naturally aligned and the tail is padded explicitly -- the header carries
 * the size and a reader refuses a file whose records are a different length,
 * which is what catches a struct that grew without the version moving.
 *
 * Deliberately not tagcache numeric tags. Those would have cost the last free
 * index in the 32-bit TAGCACHE_NUMERIC_TAGS mask (see tagcache.h) and a
 * header-version bump, which rebuilds every player's database. This file is
 * the fork's own and answers to nothing. */
struct sound_record
{
    uint64_t key;            /* sound_index_key() of the track's path */

    /* What makes a record stale. Two of them, because neither is enough on
     * its own and because they are not equally available.
     *
     * mtime is the database's, which is the FAT directory time read as local
     * time -- a value the player produces and a desktop tool cannot reliably
     * reproduce across timezones. So a tool that cannot supply it writes
     * zero, and zero means "unknown" rather than "ancient".
     *
     * size both sides compute identically, and it catches the case that
     * actually matters: a file replaced or re-encoded. Between them a record
     * is stale when the size differs, or when both mtimes are known and
     * disagree. */
    uint32_t mtime;          /* 0 = not known */
    uint32_t size;           /* Low 32 bits of the file length */

    uint32_t genre_key;      /* sound_index_genre_key(), 0 for no genre */
    uint16_t year;
    uint16_t period_ms;      /* 0 = never locked. Raw, unfolded */
    int16_t  loudness_db10;  /* dBFS x10 */
    uint8_t  confidence;
    uint8_t  tempo_spread;   /* ms between the slowest and fastest, capped */
    uint8_t  crest_db;       /* Peak over RMS, whole dB */
    uint8_t  width;
    uint8_t  level_spread;
    uint8_t  level[3];       /* low, mid, high */
    uint8_t  rate10[3];      /* Onsets a second in tenths, capped at 25.5 */
    uint8_t  strength;
    uint8_t  peakiness;
    uint8_t  analysed_s;     /* Audio the measurement covers, capped at 255 */
    uint8_t  flags;

    /* Pitch. The twelve classes are kept whole and not just the key derived
     * from them: two tracks in neighbouring keys share most of their notes,
     * which a reader comparing these can see and a key name cannot. Twelve
     * bytes against a scan that costs a night is a cheap thing to have and an
     * expensive one to add later. */
    uint8_t  pitch[12];
    uint8_t  tonic;          /* 0 = C .. 11 = B */
    uint8_t  mode;           /* 0 = major, 1 = minor */
    uint8_t  mode_margin;    /* Below CHROMA_MARGIN_MIN the mode means
                                nothing; SOUND_F_NO_MODE says so too */
    uint8_t  tonal_clarity;
    uint8_t  harmonic_change;

    uint8_t  pad[6];
};

#define SOUND_F_SETTLED  0x01  /* The tempo met beat_probe_settled() */
#define SOUND_F_NO_LOCK  0x02  /* No tempo; period_ms is 0 and means nothing */
#define SOUND_F_SHORT    0x04  /* The file ended before the window did */
#define SOUND_F_FAILED   0x08  /* The decode failed. Kept so the file is not
                                  read again during the run that failed on it;
                                  an update retries it. See sound_index.c */
#define SOUND_F_NO_MODE  0x10  /* Pitch content did not settle on a mode --
                                  common, and not a fault. See chroma.h */

/* Errors, matching db_summary's convention. */
#define SOUND_OK          0
#define SOUND_ERR_IO     -1
#define SOUND_ERR_MEM    -2
#define SOUND_ERR_NONE   -3   /* No index, and this call does not build one */

/* Keys.
 *
 * FNV-1a as system/hash.h defines it, but 64-bit and written here rather than
 * taken from there: that header says in as many words that nothing may write
 * its values to disk and expect a later build to reproduce them. These go to
 * disk and must survive a rebuild, so the arithmetic is pinned here and the
 * constants below are the standard ones. Do not "tidy" this into hash.h.
 *
 * Sixty-four bits, not thirty-two, because thirty-two collide about once in
 * every three hundred libraries of five thousand tracks -- rare enough to pass
 * testing and certain enough to happen, and the symptom is one track wearing
 * another's measurements for good.
 *
 * The path is folded to lower case on the way in. FAT does not distinguish
 * case, so the same file can come back differently cased and would otherwise
 * key to two different records. */
/* Whether a record carries a measurement at all.
 *
 * A decode that failed is written anyway, so a broken file is not retried on
 * every run, and what is written is zeroed. Zero is not neutral in any of
 * these fields -- a loudness of zero is full scale -- so a reader that treats
 * such a record as data gets a track that looks maximally loud and maximally
 * compressed, and nothing sounds like it. Ask here instead of testing the
 * flag, which is one bit of a rule with two halves. */
bool sound_record_usable(const struct sound_record *r);

/* The portion of a path the index keys by -- the same path with any volume
 * specifier removed, pointing into the caller's own string. Callers that
 * group tracks by where they sit need this too, or a path from one tagcache
 * call will not group with the same track's path from another. */
const char *sound_index_path(const char *path);

uint64_t sound_index_key(const char *path);
uint32_t sound_index_genre_key(const char *genre);

/* Fill a record from one measurement. */
void sound_index_fill(struct sound_record *out, uint64_t key, uint32_t mtime,
                      uint32_t size, uint32_t genre_key, int year,
                      const struct track_sound *s, int decode_rc);

/* ---- writing ---- */

/* Open the scan's working file and learn what is already in it.
 *
 * 'capacity' is how many tracks the caller expects to write, used to size the
 * table of what is done. 'fresh' discards any working file rather than
 * resuming it -- which is the *only* way to start over, because an unfinished
 * file otherwise always resumes. A scan that took eight hours and stopped at
 * ninety percent must not be restartable only from the beginning.
 *
 * SOUND_OK, or an error. */
int sound_index_begin(int capacity, bool fresh);

/* Whether this track is already measured and still current -- the staleness
 * rule described on the record above, in one place so the two front ends
 * cannot disagree about it. Pass 0 for an mtime that is not known. */
bool sound_index_done(uint64_t key, uint32_t mtime, uint32_t size);

/* Append one record. False if it could not be written, in which case the file
 * is left exactly as it was: records have no markers and a reader steps
 * through them at a fixed stride, so half a record would put every record
 * after it at the wrong offset with nothing to resynchronise from. */
bool sound_index_add(const struct sound_record *r);

/* Records written into the working file so far, across resumes. */
int sound_index_count(void);

/* Sort by key and put the finished index in place. */
int sound_index_finish(void);

/* Stop, keeping the working file so the next run resumes from it. */
void sound_index_close(void);

/* Whether a scan is part-finished, and how far it got. For the screen that
 * offers to continue rather than start again. */
bool sound_index_partial(int *done);

/* Whether there is a finished index to read. Cheap enough for a menu item to
 * ask on every draw, which is what it is for. */
bool sound_index_exists(void);

/* ---- reading ---- */

/* A reader holds the file open and seeks to what it is asked for, rather than
 * loading the lot: twenty thousand tracks is 800K, and taking that from core
 * stops playback to get it (see db_summary.h). */
struct sound_index_reader
{
    int fd;
    int count;
};

int  sound_index_reader_open(struct sound_index_reader *r);
void sound_index_reader_close(struct sound_index_reader *r);

/* Record n of r->count. */
bool sound_index_read(struct sound_index_reader *r, int n,
                      struct sound_record *out);

/* The record for one key, by binary search -- the file is sorted by key. */
bool sound_index_find(struct sound_index_reader *r, uint64_t key,
                      struct sound_record *out);

#endif /* _SOUND_INDEX_H */
