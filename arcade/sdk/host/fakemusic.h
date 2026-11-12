/* fakemusic: the owner's music, made up, for pbgvm and the IDE -- a small
 * library of albums, a queue playing through it, and each track's steady
 * beat for the beat service. */
#ifndef FAKEMUSIC_H
#define FAKEMUSIC_H

#include "games/arcade/arcade.h"

extern const struct arcade_beat fake_beat;
extern const struct arcade_tracks fake_tracks;

/* say: where what the cart asks of the music is reported ("seek 30000",
 * "control 1"); NULL for nowhere */
void fake_music_init(void (*say)(const char *line));

/* every track at bpm instead of its own tempo; 0 for its own */
void fake_music_tempo(int32_t bpm);

/* playing or paused; playback.playing() reports it */
void fake_music_play(int on);
int fake_playing(void *ctx);

/* playback.toggle(), next() and prev() (0, 1, 2), as the owner's remote:
 * a cart's, reported, and the host's own, not */
void fake_control(void *ctx, int what);
void fake_music_control(int what);

/* the track's clock on by ms while playing; one run out moves the queue
 * on. Then this tick's record of the music for arcade_music(). */
void fake_music_advance(int32_t ms);
void fake_music_tick(struct arcade_music *m);

/* what is playing, for a host showing it: the track's id (-1 for none),
 * the ms into it and its length, and its tempo */
int32_t fake_music_now(int32_t *ms, int32_t *length, int32_t *bpm);

#endif
