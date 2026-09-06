/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to sound_mix.c: playlists built from how tracks sound.
 ****************************************************************************/

#ifndef _SOUND_MIX_H
#define _SOUND_MIX_H

#include <stdbool.h>
#include <stdint.h>
#include "database/sound_index.h"

/* Tracks a mix may hold. The candidate arrays are three times this and the
 * running order one of them, so it is what the engine costs in static memory
 * -- about eight kilobytes here. */
#define SOUND_MIX_MAX  100

/* The scale every axis is put on. Wide enough that a squared difference keeps
 * its resolution in integers, small enough that eleven of them summed stay
 * well inside a 32-bit word. Shared, because a mood names points on these
 * same axes. */
#define SOUND_AX  1000

/* The axes a mix is judged on, derived from a record rather than stored in
 * one.
 *
 * Derived, and that is deliberate: the weighting below is the part most
 * likely to be wrong, and a scan costs half an hour. Everything here is
 * arithmetic on fields already in the record, so a better formula ships in a
 * firmware update and every existing index still answers to it.
 *
 * Each is 0-1000 against a fixed absolute range -- not normalised across the
 * library, or a track's coordinates would move when music was added. */
struct sound_axes
{
    int loud, dens, bright, low, mid, crest, width, peak, clarity, change;
    int tempo;          /* -1 where the tracker never settled on one */
    int mode;           /* -1 where the pitch content did not commit */
    uint32_t genre;
    int year;
    int energy;         /* The one summary figure, for display and sorting */
};

void sound_mix_axes(const struct sound_record *r, struct sound_axes *out);

/* How unlike each other two tracks are, 0 (identical) upwards. */
int sound_mix_distance(const struct sound_axes *a, const struct sound_axes *b);

/* Why a mix could not be built. Separated because what to do about each is
 * different: an unmeasured track wants a scan, an empty result wants a wider
 * library, and a database that is busy wants a moment. */
#define SOUND_MIX_NO_INDEX     -1   /* No index to read */
#define SOUND_MIX_NO_RECORD    -2   /* Index has nothing for this track */
#define SOUND_MIX_NO_DB        -3   /* The database would not open */
#define SOUND_MIX_NO_PLAYLIST  -4   /* Tracks were chosen, none reached the
                                       playlist */
#define SOUND_MIX_CANCELLED    -5   /* The erase warning was declined */

/* Build a playlist of tracks that sound like the one at 'path', and start it.
 * The seed plays first and the rest follow in order of how near they are to
 * it, no more than two from any one artist.
 *
 * Returns the number of tracks playing, 0 if nothing near enough survived, or
 * one of the codes above.
 *
 * Two sequential passes and no allocation: the index is read once to find the
 * nearest keys, then the database once to turn those keys back into paths.
 * The obvious alternative -- hold the index in memory -- would take it from
 * the audio buffer, and taking that stops playback to get it. A feature
 * reached from the playing screen must not stop the music to answer. */
int sound_mix_from_track(const char *path, int want);

/* Build a playlist of tracks that sit in one mood, and start it. Same rules
 * as above except that there is no seed, so nothing plays first. */
int sound_mix_from_mood(int mood, int want);

/* Build a playlist that travels from one mood to another and start it. Each
 * position in the run is filled from the tracks nearest that point along the
 * way, so the change is heard across the playlist rather than at a join. */
int sound_mix_journey(int from, int to, int want);

/* Extend the playlist now playing with more of the same, and play on from the
 * first added track. Appends rather than replaces, and refuses anything the
 * playlist already holds -- without which a continuation returns tracks that
 * played minutes ago. Returns the number added, or one of the codes above. */
int sound_mix_continue(int want);

/* Forget what built the current playlist. Called where a playlist is created,
 * which is the moment the terms behind the old one stop applying. */
void sound_mix_forget(void);

/* A playlist has run out. Called from the audio thread, so it does no more
 * than set a flag. */
void sound_mix_playlist_ended(void);

/* Whether a continuation is owed, clearing the record of it. False unless the
 * engine and Continue Playing are both on. */
bool sound_mix_continue_due(void);

#endif /* _SOUND_MIX_H */
