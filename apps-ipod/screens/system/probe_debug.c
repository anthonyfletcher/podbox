/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Runs the offline sound probe over one track and prints what it measured.
 *
 * The verification screen for track_decode.c and beat_probe.c, and the way
 * to compare the offline reading against the live one: play a track, look at
 * Debug > Beat analysis, then come here. The two analyse the same audio
 * through the same hop machine and should report the same tempo.
 *
 * Playback is stopped before the decode, because only one codec may be
 * loaded at a time.
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "lcd.h"
#include "audio.h"
#include "file.h"
#include "metadata.h"
#include "string-extra.h"
#include "audio/beat_probe.h"
#include "audio/track_decode.h"
#include "database/sound_index.h"
#include "screens/system/probe_debug.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "input/action.h"

/* The window, as the library scan will take it: past the intro, and long
 * enough to fill the tempo envelope and then hold. */
#define PD_START_PCT     15
#define PD_START_MIN_MS  5000
#define PD_START_MAX_MS  30000
/* The ceiling. A settled measurement stops well short of it -- the tempo
 * envelope fills at 17.8s and that is what the stop waits for. */
#define PD_WINDOW_MS     40000

/* The codec's view of the file. Large enough that an ordinary frame read
 * never straddles the end of it. */
#define PD_WINDOW_BYTES  (256 * 1024)

static unsigned long pd_last_draw;
static unsigned long pd_analysed;

/* Called from inside the codec, often. Doubles as the progress display,
 * since nothing else is running to draw one. */
static bool pd_abort(void)
{
    if (TIME_AFTER(current_tick, pd_last_draw + HZ / 2))
    {
        char line[32];

        pd_last_draw = current_tick;
        snprintf(line, sizeof (line), "Analysing %ld.%lds",
                 pd_analysed / 1000, (pd_analysed % 1000) / 100);
        splash(0, line);
    }

    return action_userabort(TIMEOUT_NOBLOCK);
}

static void pd_sink(const void *ch1, const void *ch2, int count,
                    const struct track_pcm *fmt, unsigned long track_ms)
{
    pd_analysed = track_ms;
    beat_probe_sink(ch1, ch2, count, fmt, track_ms);
}


bool probe_debug_screen(void)
{
    struct simplelist_info info;
    struct track_sound s;
    struct mp3entry *id3;
    char path[MAX_PATH];
    char genre[64];
    int year;
    unsigned long start_ms, length_ms, analysed = 0;
    long tick;
    int handle, rc;

    id3 = audio_current_track();
    if (id3 == NULL || id3->path[0] == '\0')
    {
        splash(HZ * 2, "Play a track first");
        return false;
    }

    /* The id3 belongs to playback and does not survive stopping it. */
    strlcpy(path, id3->path, sizeof (path));
    strlcpy(genre, id3->genre_string ? id3->genre_string : "", sizeof (genre));
    year = id3->year;
    length_ms = id3->length;

    audio_stop();

    start_ms = (length_ms / 100) * PD_START_PCT;
    if (start_ms < PD_START_MIN_MS)
        start_ms = PD_START_MIN_MS;
    if (start_ms > PD_START_MAX_MS)
        start_ms = PD_START_MAX_MS;

    if (length_ms < start_ms + 5000)
        start_ms = 0;

    handle = core_alloc(PD_WINDOW_BYTES);
    if (handle <= 0)
    {
        splash(HZ * 2, "No memory");
        return false;
    }

    pd_last_draw = current_tick;
    pd_analysed = 0;
    beat_probe_start();

    tick = current_tick;
    rc = track_decode_run(path, start_ms, PD_WINDOW_MS,
                          core_get_data(handle), PD_WINDOW_BYTES,
                          pd_sink, beat_probe_settled, pd_abort,
                          &analysed);
    tick = current_tick - tick;

    beat_probe_result(&s);
    core_free(handle);

    simplelist_info_init(&info, "Sound probe", 0, NULL);
    info.scroll_all = true;
    simplelist_reset_lines();

    simplelist_addline("rc %d  %ldms wall  %ldms audio", rc,
                       tick * 1000 / HZ, analysed);
    simplelist_addline("from %ldms  %dHz  %s", start_ms, s.samplerate,
                       s.settled ? "settled" : "NOT settled");
    simplelist_addline(" ");

    if (s.period_ms > 0)
    {
        simplelist_addline("tempo %d bpm  (%dms)", 60000 / s.period_ms,
                           s.period_ms);
        simplelist_addline("conf %d  spread %dms", s.confidence,
                           s.tempo_spread);
    }
    else
    {
        simplelist_addline("tempo: never locked");
    }

    simplelist_addline(" ");
    simplelist_addline("loud %d.%d dB  crest %d.%d dB",
                       s.loudness_db10 / 10,
                       (s.loudness_db10 < 0 ? -s.loudness_db10 : s.loudness_db10) % 10,
                       s.crest_db10 / 10, s.crest_db10 % 10);
    simplelist_addline("width %d  spread %d", s.width, s.level_spread);
    simplelist_addline(" ");
    simplelist_addline("level  lo %d  mid %d  hi %d",
                       s.level[BEAT_LOW], s.level[BEAT_MID],
                       s.level[BEAT_HIGH]);
    simplelist_addline("onsets lo %d.%d mid %d.%d hi %d.%d /s",
                       s.rate10[BEAT_LOW] / 10, s.rate10[BEAT_LOW] % 10,
                       s.rate10[BEAT_MID] / 10, s.rate10[BEAT_MID] % 10,
                       s.rate10[BEAT_HIGH] / 10, s.rate10[BEAT_HIGH] % 10);
    simplelist_addline("hit %d  peak %d", s.strength, s.peakiness);
    {
        static const char *nm[12] = { "C", "C#", "D", "D#", "E", "F",
                                      "F#", "G", "G#", "A", "A#", "B" };

        if (s.chroma.margin >= CHROMA_MARGIN_MIN)
            simplelist_addline("key %s %s  margin %d", nm[s.chroma.key],
                               s.chroma.minor ? "minor" : "major",
                               s.chroma.margin);
        else
            simplelist_addline("key: ambiguous (margin %d)",
                               s.chroma.margin);

        simplelist_addline("clarity %d  chg %d  frames %d",
                           s.chroma.clarity, s.chroma.change,
                           s.chroma.frames);
    }
    simplelist_addline("windows %d", s.windows);

    {
        struct track_decode_stats st;

        track_decode_get_stats(&st);
        simplelist_addline(" ");
        simplelist_addline("codec status %d", st.codec_status);
        simplelist_addline("req %u  short %u  refill %u  seek %u",
                           st.requests, st.short_answers, st.refills,
                           st.seeks);
        simplelist_addline("pos %lu of %lu", st.last_pos, st.file_len);
        simplelist_addline("boost %d  cmds %u", st.boost, st.commands);
    }

    /* Round-trip one record through the index, which is the whole of stage
     * three exercised. It writes a one-record file over any real index --
     * there is no library scan yet to lose. */
    {
        struct sound_record rec, back;
        struct sound_index_reader rd;
        uint64_t key = sound_index_key(path);

        sound_index_fill(&rec, key, 0, 0, sound_index_genre_key(genre),
                         year, &s, rc);

        simplelist_addline(" ");

        if (sound_index_begin(1, true) == SOUND_OK &&
            sound_index_add(&rec) &&
            sound_index_finish() == SOUND_OK &&
            sound_index_reader_open(&rd) == SOUND_OK)
        {
            if (sound_index_find(&rd, key, &back))
            {
                simplelist_addline("index ok: %d rec, %d bytes", rd.count,
                                   (int)sizeof (rec));
                simplelist_addline("  back %dms  %ddb  yr %d  flags %02x",
                                   back.period_ms, back.loudness_db10 / 10,
                                   back.year, back.flags);
                simplelist_addline("  %s",
                                   memcmp(&rec, &back, sizeof (rec)) == 0
                                   ? "identical" : "DIFFERS");
            }
            else
            {
                simplelist_addline("index: record LOST");
            }

            sound_index_reader_close(&rd);
        }
        else
        {
            simplelist_addline("index: write FAILED");
        }
    }

    return simplelist_show_list(&info);
}
