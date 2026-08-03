/***************************************************************************
 * Original code from RockBox
 * was: apps/plugins/lrcplayer.c
 * Synchronised lyrics: finding, loading and the document model.
 *
 * Copyright (C) 2008-2009 Teruaki Kawashima
 * GNU General Public License (version 2+)
 *
 * Loads the timed lyrics belonging to a track and answers two questions:
 * what is line N, and which line should be on screen at time T.
 *
 * Only synchronised lyrics are handled -- every line carries a timestamp, so
 * there is no guessing at scroll speed and no unsynchronised source. Plain
 * .txt lyrics and ID3 USLT frames are therefore not read.
 ****************************************************************************/

#ifndef _LYRICS_H
#define _LYRICS_H

#include <stdbool.h>
#include "metadata.h"       /* struct mp3entry */

/* Returned by lyrics_index_at() for a time before the first line. */
#define LYRICS_NONE (-1)

/* Why a load did not produce lyrics. Worth telling apart: only one of these
 * means "this track has none", and the caller's response differs -- NO_TRACK
 * is transient and worth retrying, the others are not. */
enum lyrics_result
{
    LYRICS_OK = 0,
    LYRICS_NO_TRACK,     /* no usable path yet: metadata still buffering */
    LYRICS_NO_MEMORY,    /* the working buffer could not be allocated */
    LYRICS_NOT_FOUND     /* searched, and this track really has none */
};

/* Search for and load the lyrics belonging to a track, replacing whatever was
 * loaded before. Looks for a lyrics file beside the audio file and in each
 * parent directory up to the root, then falls back to an ID3v2 SYLT frame in
 * the file itself. */
enum lyrics_result lyrics_load(const struct mp3entry *id3);

/* Release the loaded lyrics and the buffer behind them. Every pointer handed
 * out by lyrics_text() and lyrics_word_text() dies here. */
void lyrics_close(void);

bool lyrics_loaded(void);

/* The path lyrics were read from -- a lyrics file, or the audio file itself
 * for an embedded SYLT frame. Empty string when nothing is loaded. */
const char *lyrics_file(void);

/* The [ti:] and [ar:] tags, NULL unless the file names something the track's
 * own metadata does not already say. */
const char *lyrics_title(void);
const char *lyrics_artist(void);

/* --- Access by index. Indices run 0 .. lyrics_count()-1, ordered by time.
 * Out-of-range indices give NULL or 0 rather than misbehaving. --- */

int lyrics_count(void);

/* The whole text of a line, with the timing tags stripped out. */
const char *lyrics_text(int index);

/* When a line starts, and when it gives way to the next one. Both are
 * milliseconds into the track, with the file's [offset:] applied. The last
 * line ends with the track. */
long lyrics_time(int index);
long lyrics_end_time(int index);

/* --- Access by time --- */

/* The line that should be showing at `time` (ms into the track): the last one
 * whose start time has passed. LYRICS_NONE before the first line starts. */
int lyrics_index_at(long time);

/* --- Word timing, for lines written as <mm:ss.xx>word<mm:ss.xx>word.
 * Lines without it read as a single word. Words are indexed in reading
 * order, and their text is a tail of the line's, so drawing word w means
 * drawing lyrics_text() up to where lyrics_word_text(w) begins. --- */

int lyrics_word_count(int index);
const char *lyrics_word_text(int index, int word);

/* When a word starts, or -1 if it is untimed and starts with its line. */
long lyrics_word_time(int index, int word);

#endif /* _LYRICS_H */
