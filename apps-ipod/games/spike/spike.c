/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Spike screen: the clock the grid is cut from, the window a press
 * is judged in, and the loop that drives both.
 *
 * The clock is spike_clock.c's, and it is the audio's rather than the
 * tick counter's. That is the whole risk of the design: the beat index is
 * computed from it, so a clock that runs slow means the grid is not the
 * music.
 *
 * The grid is cut from that clock at a tempo the tracker supplies, latched
 * once at the end of the count-in and never revisited. Latching is the whole
 * of it: the underlying estimate can still move, and roughly one track in
 * seven changes its mind, but the player's internal clock is the game's most
 * important asset and a grid that shifted under them would be worse than a
 * grid that is slightly wrong. The pause menu carries a half/double toggle
 * for the one error nobody catches automatically.
 *
 * Parts, in order:
 *   - the tempo
 *   - the grid
 *   - input
 *   - the run
 *   - the screen loop
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "system.h"           /* TIME_AFTER, TIME_BEFORE */
#include "lcd.h"
#include "font.h"
#include "kernel.h"
#include "button.h"           /* button_hold */
#include "backlight.h"      /* backlight_set_timeout(_plugged) */
#include "file.h"             /* MAX_PATH */
#include <string.h>
#include "string-extra.h"     /* strlcpy */
#include "audio.h"            /* audio_status, audio_ff_rewind */
#include "audio/playback.h" /* audio_pre_ff_rewind */
#include "audio/beat_analyse.h"
#include "audio/beat_track.h"
#include "input/action.h"
#include "system/activity.h"
#include "system/shutdown.h"  /* default_event_handler */
#include "draw/viewport.h"    /* viewportmanager_theme_enable */
#include "lang.h"
#include "settings/settings.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "screens/playback/wps.h"   /* wps_do_playpause, DEFAULT_SKIP_THRESH */
#include "system/volume.h"   /* adjust_volume */
#include "games/spike/spike.h"
#include "games/spike/spike_score.h"
#include "games/spike/spike_bar.h"
#include "games/spike/spike_clock.h"
#include "games/spike/spike_draw.h"
#include "games/spike/spike_gen.h"
#include "games/spike/spike_menu.h"
#include "games/spike/spike_music.h"
#include "games/spike/spike_text.h"
#include "games/spike/spike_pose.h"
#include "games/spike/spike_world.h"

/* A press inside this of a boundary is that beat's; anything else belongs
 * to the next one, which is usually fatal -- that is the game. */
#define SPK_WINDOW_MS        110

/* The beats a death costs. */
#define SPK_SKIP_BEATS       4

/* Beats of slack on the end of the outro test. The assembler is asked
 * whether the pattern it is *about* to lay would still be under the player
 * when the music stops, so the only margin needed is the length of that
 * pattern and a bar to walk it off in. */
#define SPK_OUTRO_SLACK      (SPK_PAT_MAX + 4)

/* Beats of listening before the analyser is started again. A cursor that has
 * fallen behind reseeds itself, but a tracker that has taken a bad first
 * estimate has nothing to make it reconsider -- and the screen would sit on
 * "waiting for the beat" for the whole track saying nothing. */
#define SPK_RELISTEN_BEATS   24

/* Frame period in ticks, and the range it may take: 33fps down to 12fps.
 * HZ is 100, so a tick is 10ms.
 *
 * The loop waits until a deadline rather than for a period after its work.
 * Waiting afterwards makes the period the work plus the timeout, so it
 * varies with whatever the frame happened to cost, and an uneven period is
 * an uneven scroll however exact the clock is. */
#define SPK_FRAME_MIN        (HZ / 33)
#define SPK_FRAME_MAX        (HZ / 12)
#define SPK_RATE_TICKS       HZ

/* Beats the grid may be behind before the jump is treated as a seek rather
 * than a slow frame. */
#define SPK_JUMP_BEATS       4

/* How long playback must have been stopped before the run is called over.
 * Long enough that nothing which recovers on its own -- a rebuffer, a
 * handover between tracks -- can end a run. */
#define SPK_STOPPED_TICKS   (HZ / 2)

/* The results screen has no music to keep time with -- the playlist may
 * have ended -- so its gymnastics run on wall time at the grid's own
 * target tempo. */
#define SPK_RESULT_BEAT     (HZ / 2)

/* What things are worth, and what a death costs.
 *
 * Score what the player *did*, not what the track did to them. Nothing is
 * paid for surviving a beat: that pays out for the metronome ticking, which
 * a flat run does on its own, and it makes a long safe stretch worth more
 * than a short dangerous one. What is paid for is an obstacle avoided, a
 * diamond taken and a creature defeated.
 *
 * A creature is five diamonds. A diamond is mostly a matter of being
 * somewhere; a creature has to be landed on from above, off a press timed
 * to the beat, and walking into it ends the run. The two are not the same
 * kind of thing and should not be within sight of each other.
 *
 * The multiplier is a precision combo: presses that land on the beat build
 * it, and it is the whole of why playing in time scores better than merely
 * surviving. It needs no design of its own -- a press only happens to clear
 * an obstacle, so the combo *is* a measure of how well-timed the player's
 * obstacle-clearing was. Surviving is possible anywhere in the window; the
 * multiplier is only possible at the middle of it.
 *
 * A death costs four diamonds and the combo. It has to be the largest number
 * here or surviving badly scores as well as playing well, which is the one
 * thing the table exists to separate. */
#define SPK_PTS_AVOID        50
#define SPK_PTS_DIAMOND      100
#define SPK_PTS_STOMP        500
#define SPK_PTS_PRESS        25
#define SPK_PTS_DEATH        1000
/* Every diamond in the phrase. It is the largest number in the table on
 * purpose: it is what makes a bait cost something, since a bait's two routes
 * are authored so that neither can take them all. */
#define SPK_PTS_PHRASE       500
#define SPK_MULT_MAX         8

/* A press this close to the beat builds the combo.
 *
 * Trap: 45 rather than the 35 the design first wanted, because button
 * events are posted from a 100Hz tick task and so are quantised to 10ms
 * whatever they are timestamped against. Reading USEC_TIMER here times when
 * the press was noticed, not when it happened. */
#define SPK_PRECISE_MS       45

/* Where in a beat the leading edge of the body meets whatever is standing
 * in the next cell -- about twelve of the thirty-two pixels, which the
 * front-loaded ease reaches early. A contact death fires here rather than at
 * the boundary: by the boundary the player has walked the whole cell and is
 * standing on the thing, and a death that starts from there is describing
 * something that already finished. */
#define SPK_TOUCH_PHASE      37

/* Octave shifts the pause menu offers on top of the latched tempo. A doubled
 * estimate is the one error a player identifies instantly and no algorithm
 * reliably catches, so it is worth a button. */
#define SPK_SHIFT_MIN        (-1)
#define SPK_SHIFT_MAX        1

enum spk_run
{
    SPK_RUN_COUNT,       /* walking on, and waiting for a tempo */
    SPK_RUN_PLAY,
    SPK_RUN_DEAD,        /* one beat, frozen, playing the death */
    SPK_RUN_SKIP,        /* dimmed, on the way to a respawn */
    SPK_RUN_OVER         /* the mode says this run has ended */
};

/* Kept across entries so a calibration survives leaving the screen. It is
 * not a saved setting yet: a value that only a debug-menu screen reads has
 * no business in config.cfg until the game has a menu of its own.
 *
 * The default is where tapping put it rather than zero. The tracker's beats
 * land late against what is heard -- measured at about -118ms by tapping,
 * of which some is the analyser's phase and some is a player's habit of
 * anticipating -- and -50 is where it started feeling right in play. */
static int  offset_ms = -50;

/* The latched grid, and the octave the player asked for on top of it. The
 * shift is kept across entries with the offset, for the same reason. */
static int  beat_ms = SPK_BEAT_TARGET;
static int  tempo_shift;
static int  tempo_bpm;          /* 0 where the tempo was never locked */
static int  tempo_conf;         /* what the tracker thought of it */
static int  bar_rot;            /* the downbeat's residue of four, -1 unknown */

static long anchor_ms;              /* grid time zero, in clock terms */
static int  cur_beat;
static enum spike_mode play_mode;
static enum spk_run run_state;
static long run_beats;              /* how far the run has come */
static long stopped_at;             /* tick playback was first seen stopped */
static bool run_began;              /* ...and whether it ever did */
static enum spk_death death_kind;
static long death_at;           /* grid time the death began */
static int  death_scroll;       /* ...and where the scroll had got to */
static long last_grid;          /* grid time as of the last frame */
static int  listened;           /* beats spent waiting for a tempo */
static int  respawn_cell;

static struct spk_state world;

/* A press that arrived outside its beat's window and so belongs to the
 * next one. */
static int  pending_beat;

/* The calibration read-out: signed error of every press, so the player can
 * null their own bias out along with the output delay. */
static long score;
static int  combo;

static int  phrase_at;      /* first cell of the phrase being crossed */
static int  phrase_got;     /* diamonds taken in it so far */

static long press_total;
static int  press_count;
static int  press_last;

/* What this run has to beat, read once on the way in. Run's is one number
 * for the player; Song's belongs to the track, so the path is taken at
 * entry -- in Song the track cannot change without ending the attempt. */
static long best_score;
static long best_beats;
static char best_path[MAX_PATH];


/** The screen furniture **/

/* Hold the backlight for the run, and only override a timeout that was
 * actually running. */
static void spike_backlight(bool hold)
{
    if (hold)
    {
        if (global_settings.backlight_timeout > 0)
            backlight_set_timeout(0);
        if (global_settings.backlight_timeout_plugged > 0)
            backlight_set_timeout_plugged(0);
    }
    else
    {
        backlight_set_timeout(global_settings.backlight_timeout);
        backlight_set_timeout_plugged(
            global_settings.backlight_timeout_plugged);
    }
}

/* wps_do_playpause() toggles, so asking for a state it is already in would
 * do the opposite of what was asked. */
static void spike_set_paused(bool pause)
{
    bool now = (audio_status() & AUDIO_STATUS_PAUSE) != 0;

    if (now != pause)
        wps_do_playpause(false);
}


/** The tempo **/

/* Re-anchor so the boundary after this one falls on 'at', keeping the beat
 * index continuous. Everything that changes the grid goes through here: the
 * latch, and the half/double toggle. */
static void spike_anchor(long at)
{
    anchor_ms = at - offset_ms - (long)(cur_beat + 1) * beat_ms;
}

static int spike_shifted(int ms)
{
    return tempo_shift < 0 ? ms >> -tempo_shift : ms << tempo_shift;
}

/* Take the tempo once and keep it. Called at the end of the count-in and at
 * nothing else, which is what makes the grid something the player can learn. */
static bool spike_latch_tempo(void)
{
    struct beat_track bt;
    unsigned long beat_at;

    beat_track_get(&bt);
    tempo_conf = (int)bt.confidence;

    /* Only ever reached with a lock, and it can still refuse. §0.5 leaves
     * what to do without a tempo open; the answer here is to wait rather
     * than to start, because a grid that is not the music's is not a worse
     * game but a different one. Every judgement the player makes is against
     * a beat they can hear, and a free-running grid asks them to ignore it.
     *
     * The tracker's own verdict is taken as it stands, with no second
     * threshold on top: its confidence is an autocorrelation ratio out of
     * 100 and real music sits in the tens -- beat_track.c locks at 12, after
     * several estimates in a row agree. A higher bar here rejected good
     * locks at 38 and 42. */
    beat_ms = spk_octave(bt.period_ms);
    tempo_bpm = (int)bt.bpm;

    /* Out of range is an estimate that was never usable, and there is
     * nothing to fall back to that would be in time. Wait for another. */
    if (beat_ms < SPK_BEAT_MIN || beat_ms > SPK_BEAT_MAX || tempo_bpm <= 0)
    {
        tempo_bpm = 0;
        return false;
    }

    beat_ms = spike_shifted(beat_ms);

    /* The course is the song's, and this is the only thing about the song
     * the generator is told. Latched with the tempo and never revisited. */
    spk_gen_set_seed(beat_ms);

    /* Nothing reads the tracker again. The tempo is latched for the run by
     * §0.5, so the analyser has done its whole job here -- and it is by far
     * the most expensive thing on the frame, because its rate is set by the
     * audio (172 hops a second) rather than by how often the screen draws.
     * Left running it costs the same whether the game manages 30 frames a
     * second or 7, which on the 5G is the difference between the two. */
    beat_analyse_stop();

    /* Beats are projected rather than waited for, so the grid can be laid
     * against one that has not been heard yet -- which is the only way the
     * first beat of the run can be on the beat. */
    if (beat_track_next(spk_clock_ms(), 0, &beat_at, NULL))
        spike_anchor((long)beat_at);
    else
        spike_anchor((long)spk_clock_ms() + beat_ms);

    /* And the bar, from the onsets the wait recorded.
     *
     * It moves the phrase boundaries so that a phrase begins where a bar
     * does, and it moves nothing else. The distinction is the whole reason
     * it can be used at all: the downbeat is measured from whatever history
     * happened to exist when the tracker locked, so a run that locked
     * quickly finds no bar where a slow one finds bar three -- and if the
     * rotation reached the *choice* of phrase, those two runs would be
     * different levels. The generator takes it out of the cell before
     * deciding anything, so they are the same phrases in the same order,
     * offset by up to three cells and by nothing else. */
    bar_rot = spk_bar_downbeat(anchor_ms + offset_ms, beat_ms);
    spk_gen_set_bar(bar_rot < 0 ? 0 : bar_rot);

    return true;
}


/** The grid **/

/* Whether the track under a Song attempt is still the one it was against.
 *
 * The clock says the audio moved; only the path says it moved to something
 * else. Ending an attempt is the one thing here that cannot be undone, so
 * it is worth a second opinion -- everything that can jog the clock without
 * changing the track (a rebuffer, a stall the game was not running through)
 * stops being able to end a run.
 *
 * With no path to compare -- a database row whose file was never opened --
 * the clock is all there is, which is where this started. */
static bool spike_track_changed(void)
{
    struct mp3entry *id3 = audio_current_track();

    if (best_path[0] == '\0')
        return true;

    return id3 == NULL || id3->path == NULL
           || strcmp(id3->path, best_path) != 0;
}

/* Grid time, which is clock time with the run's origin and the output delay
 * taken out. A positive offset says the sound leaves the socket that much
 * after the position reports it, so the beats the player is judged against
 * move later by the same amount. */
static long spike_now(void)
{
    return (long)spk_clock_ms() - anchor_ms - offset_ms;
}

/* Back to listening, without stopping. Everything the audio invalidates goes
 * -- the tempo it was cut from, the analyser's history -- and everything the
 * player can see stays: the cell the world is on, the ground under it, the
 * walk. A track change is a track change and not a new game.
 *
 * The tempo is left standing rather than reset to the target, so the walk
 * keeps the rhythm it had until there is a better one. The grid is
 * re-anchored a whole beat out, which keeps the beat index continuous across
 * a clock that has just jumped back to a new track's zero. */
static void spike_listen(int cell)
{
    tempo_bpm = 0;
    tempo_conf = 0;
    bar_rot = -1;
    listened = 0;
    pending_beat = -1;
    combo = 0;

    /* The bar belongs to the track it was heard in. */
    beat_analyse_start();
    spk_bar_reset();
    spk_music_reset();
    spk_gen_set_bar(0);

    /* Carry the grid straight across the join. Anchoring a whole beat out
     * keeps the beat *index* continuous and throws the phase away, which on
     * a track change is a scroll that jumps back part of a cell for no
     * reason the player can hear. Anchoring on the grid time the last frame
     * had keeps both. */
    anchor_ms = (long)spk_clock_ms() - offset_ms - last_grid;

    /* Flat first, then the cells: patterns are laid the moment one is asked
     * for, and starting the player is what asks. Setting the mode afterwards
     * leaves the phrase the player is standing in as whatever the assembler
     * would have chosen -- which is where the diamonds at the start of a run
     * were coming from.
     *
     * And the body is re-seated, standing, on the cell it is already on. A
     * track change can arrive with the player mid-jump or one beat from a
     * hole, and `spk_state_advance()` does not advance a state whose move
     * ends the line -- so a world left in one sits there for the whole wait,
     * looping the pose it was in and never reaching another boundary. */
    spk_gen_reset(cell);
    spk_gen_set_flat(true);
    spk_state_start(&world, cell);
    phrase_at = spk_gen_pattern_start(cell);
    phrase_got = -1;

    run_state = SPK_RUN_COUNT;
}

/* A new game: the screen has just been entered, or the clock has moved in a
 * way that means the world on it was never the player's. */
static void spike_reset_run(void)
{
    beat_ms = spike_shifted(SPK_BEAT_TARGET);
    spk_draw_reset();

    anchor_ms = (long)spk_clock_ms() - offset_ms;
    last_grid = 0;
    cur_beat = 0;
    death_kind = SPK_DEATH_LEDGE;
    respawn_cell = 0;
    phrase_got = 0;
    score = 0;
    run_beats = 0;
    run_began = false;
    stopped_at = 0;

    /* The course is cut from where the music is, not from where the run is:
     * the cell index is the track's own beat count, so starting a song
     * half-way through picks the course up there and two runs of the same
     * song are not the same course. Measured against the provisional grid,
     * which is close enough -- what it has to be is different for different
     * places in the track, not exact. */
    spike_listen((int)(spk_clock_ms() / (unsigned long)beat_ms));
}

/** Input **/

/* Attribute a press to a beat and say how far off it was. The rule is one
 * line: the nearest boundary if the press falls inside its window, and the
 * next one otherwise. A press in the dead middle of a beat is therefore
 * buffered rather than dropped, and repeats inside one window collapse
 * because a jump already under way refuses the second. */
static void spike_press(long press_ms)
{
    long beat, frac, target, err;

    if (press_ms < 0)
        return;

    beat = press_ms / beat_ms;
    frac = press_ms % beat_ms;

    if (frac <= SPK_WINDOW_MS)
    {
        target = beat;
        err = frac;
    }
    else
    {
        target = beat + 1;
        err = frac - beat_ms;
    }

    press_last = (int)err;
    press_total += err;
    press_count++;

    if (run_state != SPK_RUN_PLAY)
        return;

    /* Surviving is possible anywhere in the window; scoring well is not.
     * That difference is what keeps the game rhythmic rather than merely
     * turn-based, so it is paid on the press and not on the outcome. */
    if (err <= SPK_PRECISE_MS && err >= -SPK_PRECISE_MS)
    {
        score += SPK_PTS_PRESS;
        combo++;
    }

    if (target == cur_beat)
    {
        /* Late inside the window. The jump is still committed to this beat:
         * the arc is drawn at its true phase, so the take-off is clipped by
         * however late the press was and the landing falls exactly on the
         * boundary regardless. That is where a mistimed press is paid for,
         * and it is the only place -- the arrival is one of two poses
         * whatever the flight looked like. */
        spk_state_jump(&world);
    }
    else if (target == cur_beat + 1)
        pending_beat = (int)target;
}


/** The run **/

static int spike_multiplier(void)
{
    int m = 1 + (combo >> 3);

    return m > SPK_MULT_MAX ? SPK_MULT_MAX : m;
}

/* 'scroll' is where the world had scrolled to when this happened, in 256ths
 * of a cell: the field freezes there and nowhere else. A boundary death has
 * crossed the whole cell and passes SPK_PHASE; a contact is noticed part-way
 * through and passes what the phase had actually reached. */
static void spike_die(enum spk_death kind, long at, int scroll)
{
    run_state = SPK_RUN_DEAD;
    death_kind = kind;
    death_at = at;
    death_scroll = scroll;
    pending_beat = -1;

    /* The combo is the run's memory of playing in time, so a death has to
     * take it: keeping it would make surviving badly as good as playing
     * well, which is the one thing the scoring is there to separate.
     *
     * Song pays no points, because the death already costs it the rest of
     * the track. There is one life a track, so what dying takes is every
     * point that was still to come -- charging 1000 on top would charge the
     * same death twice. */
    combo = 0;
    if (play_mode == SPIKE_MODE_RUN)
    {
        score -= SPK_PTS_DEATH;
        if (score < 0)
            score = 0;
    }

    /* And nothing else. A death costs points and the combo; it does not
     * touch the course. The course belongs to the song, and a player who
     * keeps dying has to be able to meet the same phrase again until they
     * get past it. spk_world_respawn() finds the safe ground the forced rest
     * used to provide. */
    phrase_got = -1;
}

/* Cross one beat boundary. Called once per boundary and never in bulk: a
 * clock that has jumped is a seek, and is handled as one. */
static void spike_boundary(long grid_ms)
{
    switch (run_state)
    {
    case SPK_RUN_COUNT:
    {
        struct beat_track bt;

        /* The world walks through the wait on flat ground. The wait is
         * open-ended, so the course cannot be laid through it -- but holding
         * the field still and the body in a pose of its own means the run
         * starts with a jolt, and what the player is being asked to feel at
         * that moment is a tempo. So: the assembler is held to rests, and
         * the walk carries straight on into the course when it arrives. */
        spk_state_advance(&world);

        beat_track_get(&bt);

        if (bt.locked && spike_latch_tempo())
        {
            /* The cell index is the track's own beat count, and it can only
             * be worked out now: until the tempo is latched there is no beat
             * to count. Measured against the provisional grid it would drift
             * from the music at the ratio of the two tempos, and then the
             * course a player met at 1:30 would depend on when they started
             * the game rather than on where the song was -- which is the
             * whole of what a fixed course means.
             *
             * Re-seating the world here is invisible: the ground is flat on
             * both sides of it, so only the number changes. */
            int cell = (int)(spk_clock_ms() / (unsigned long)beat_ms);

            spk_gen_reset(cell);
            spk_gen_set_flat(true);
            spk_state_start(&world, cell);

            /* One more phrase of flat ground beyond the player's own, so the
             * course begins about where the field's right edge is and
             * scrolls in rather than appearing under their feet. */
            spk_gen_cell(spk_gen_next_cell());
            spk_gen_set_flat(false);

            phrase_at = spk_gen_pattern_start(world.beat);
            phrase_got = -1;
            run_began = true;
            run_state = SPK_RUN_PLAY;
            break;
        }

        /* Long enough to have heard the whole of a phrase several times
         * over. Whatever the tracker has settled on is not going to
         * improve, so it is given the track again from here. */
        if (++listened >= SPK_RELISTEN_BEATS)
        {
            listened = 0;
            beat_analyse_start();
        }
        break;
    }

    case SPK_RUN_PLAY:
    {
        /* Before the move is applied, because it asks what the *other* move
         * would have done and the other move starts from here. */
        bool avoided = spk_state_avoided(&world);
        enum spk_outcome out = spk_state_advance(&world);
        int mult;

        /* What Run reports beside the score, because "how long did you
         * last" is the question that mode is asking and a score alone does
         * not answer it. Counted here rather than from the cell index: the
         * cell is the track's beat count and restarts with every track. */
        run_beats++;

        /* A contact has already fired, mid-beat, at the moment the two
         * met -- see the touch test in the loop. Reaching the boundary with
         * one still outstanding would mean the phase never got there, which
         * a very slow frame can do. */
        if (out == SPK_HIT || out == SPK_CRUSHED || out == SPK_BONKED)
        {
            spike_die(SPK_DEATH_OUCH, grid_ms, SPK_PHASE);
            break;
        }

        if (out == SPK_FELL)
        {
            spike_die(world.motion == SPK_WALK
                         ? SPK_DEATH_LEDGE : SPK_DEATH_AIR, grid_ms, SPK_PHASE);
            break;
        }

        /* Phrases are the generator's, not a fixed number of beats: the
         * library holds patterns of four cells and of eight. */
        if (spk_gen_pattern_start(world.beat) != phrase_at)
        {
            int had = spk_gen_pattern_diamonds(phrase_at);

            /* The phrase just left, before this beat's own diamond is
             * counted -- that one belongs to the phrase now beginning. */
            if (had > 0 && phrase_got >= had)
                score += SPK_PTS_PHRASE * spike_multiplier();

            phrase_got = 0;
            phrase_at = spk_gen_pattern_start(world.beat);
        }

        /* The bar just heard, handed to the assembler before it is asked
         * for any more cells. It runs a phrase or two ahead of the field,
         * so what a phrase is chosen on is the music of a phrase or two
         * ago -- which §9.4 says is exactly right: a pattern is committed
         * long before it is reached, and no forward view into the audio
         * could reach that far anyway. */
        {
            struct spk_mood mood;

            spk_music_beat();
            spk_music_get(&mood);
            spk_gen_set_mood(mood.energy, mood.flux, mood.trend);
        }

        mult = spike_multiplier();

        if (avoided)
            score += SPK_PTS_AVOID * mult;

        if (world.got)
        {
            score += SPK_PTS_DIAMOND * mult;
            phrase_got++;
        }

        if (world.stomped)
            score += SPK_PTS_STOMP * mult;

        if (pending_beat == cur_beat)
        {
            spk_state_jump(&world);
            pending_beat = -1;
        }
        break;
    }

    case SPK_RUN_DEAD:
        /* The death runs on its own clock now, because it can start
         * part-way through a beat. The loop ends it. */
        break;

    case SPK_RUN_OVER:
        /* It crosses no boundary: the loop stops counting them the moment
         * the run leaves the player's hands. */
        break;

    case SPK_RUN_SKIP:
        world.beat++;
        if (world.beat >= respawn_cell)
        {
            spk_state_start(&world, world.beat);

            /* The run rejoins part-way through a phrase, so the phrase it
             * lands in was never crossed and cannot be cleared. */
            phrase_at = spk_gen_pattern_start(world.beat);
            phrase_got = -1;
            run_state = SPK_RUN_PLAY;
        }
        break;
    }
}


/** The screen **/

static void spike_fill_frame(struct spk_frame *f, long grid_ms)
{
    long sub = grid_ms % beat_ms;

    if (sub < 0)
        sub = 0;

    f->st = &world;
    f->phase = (int)((sub * SPK_PHASE) / beat_ms);
    f->now_ms = grid_ms > 0 ? (unsigned long)grid_ms : 0;
    f->strong = (cur_beat & 1) == 0;
    f->skipping = run_state == SPK_RUN_SKIP;
    f->score = score;
    f->multiplier = spike_multiplier();

    /* Only against a best that exists. A crown from the first point of a
     * first run says nothing: there is no record to be past. */
    f->crowned = best_score > 0 && score > best_score;
    f->waiting = run_state == SPK_RUN_COUNT;

    f->death_kind = death_kind;
    if (run_state == SPK_RUN_DEAD)
    {
        long age = (grid_ms - death_at) * SPK_PHASE / beat_ms;

        f->death_phase = (int)(age < 0 ? 0
                               : age > SPK_PHASE - 1 ? SPK_PHASE - 1 : age);
    }
    else
        f->death_phase = -1;

    f->death_scroll = death_scroll;
    f->frozen = f->death_phase >= 0;
}

/* Leaving costs a run, so it is asked rather than taken -- Menu is one
 * button away from the jump and a run is twenty minutes of work. Run only:
 * Song is a track, and the attempt it discards is a minute at most.
 *
 * The dialog draws through the skin engine, so the game's display state is
 * handed back for it exactly as it is for the menu. */
static bool spike_confirm_exit(void)
{
    static const char *lines[] = { ID2P(LANG_SPIKE_LEAVE) };
    static const struct text_message message = { lines, 1 };
    struct viewport vp;
    enum yesno_res res;

    lcd_setfont(FONT_UI);
    viewportmanager_theme_undo(SCREEN_MAIN, true);

    res = gui_syncyesno_run(&message, NULL, NULL);

    viewportmanager_theme_enable(SCREEN_MAIN, false, &vp);
    lcd_set_backdrop(NULL);
    lcd_setfont(FONT_SYSFIXED);
    spk_draw_full_flush();

    return res == YESNO_YES || res == YESNO_USB;
}

/* Play, and only play. Pausing is something a player does mid-run and wants
 * over with; everything the game can be told is on held Menu instead.
 *
 * The field does not have to be redrawn to freeze: the clock is the audio's
 * and a stopped report stops the grid. Returns true where the player left
 * from here. */
static bool spike_paused(bool by_hold)
{
    bool quit = false;

    /* The player's own splash, not a box of the game's: it is the same
     * word in the same place it appears everywhere else on the machine,
     * and there is nothing here worth a look of its own. Drawn once and
     * left standing -- nothing else draws while the field is stopped. */
    splash(0, ID2P(LANG_PAUSE));

    while (1)
    {
        int button = get_action(CONTEXT_SPIKE, HZ / 4);

        /* A pause the hold switch forced ends when the switch does, and can
         * end no other way: no button reaches here while it is on. */
        if (by_hold)
        {
            if (!button_hold())
                break;
            continue;
        }

        /* Asked here as well as on the field. A run is no less lost for
         * being paused when the button was pressed, and Menu is no further
         * from the jump. */
        if (button == ACTION_SPIKE_EXIT)
        {
            if (play_mode != SPIKE_MODE_RUN || spike_confirm_exit())
            {
                quit = true;
                break;
            }

            splash(0, ID2P(LANG_PAUSE));
            continue;
        }

        if (button == ACTION_SPIKE_PAUSE || button == ACTION_SPIKE_JUMP)
            break;

        default_event_handler(button);
    }

    spike_set_paused(false);
    spk_draw_full_flush();

    return quit;
}

/* The game's menu, which is the player's menu code and not the game's: a
 * list of options is a list and the theme already knows how to draw one.
 * The field is drawn by hand because it is a field.
 *
 * The whole of the game's display state is handed back for it -- the theme,
 * the font, the backdrop -- and taken again afterwards, because the menu
 * draws through the skin engine and the game has turned all of that off.
 *
 * Returns true where the menu was left for the root, which the game passes
 * on rather than swallowing. */
static bool spike_menu(int fps, int draw_ms, int flush_ms)
{
    struct viewport vp;
    struct spk_menu m;
    bool root;

    m.offset_ms = &offset_ms;
    m.shift = &tempo_shift;
    m.beat_ms = beat_ms;
    m.bpm = tempo_bpm;
    m.bar = bar_rot;
    m.mean_ms = press_count ? (int)(press_total / press_count) : 0;
    m.presses = press_count;
    m.fps = fps;
    m.draw_ms = draw_ms;
    m.flush_ms = flush_ms;

    /* What the tracker is making of it, which only means anything while it
     * is still being waited for: after the latch the analyser is stopped
     * and its window count stands still. A screen that only says "waiting"
     * cannot tell a tracker working slowly from one that has stopped, and
     * that difference is the whole of what a report of "it never found it"
     * needs to settle. */
    m.waiting = run_state == SPK_RUN_COUNT;
    m.listen_beats = listened;
    m.listen_conf = 0;
    m.listen_windows = 0;
    m.clock_ms = spk_clock_ms();

    if (m.waiting)
    {
        struct beat_track bt;
        struct beat_status bs;

        beat_track_get(&bt);
        beat_analyse_status(&bs);
        m.listen_conf = (int)bt.confidence;
        m.listen_windows = bs.windows;
    }

    lcd_setfont(FONT_UI);
    viewportmanager_theme_undo(SCREEN_MAIN, true);

    root = spike_menu_show(&m);

    viewportmanager_theme_enable(SCREEN_MAIN, false, &vp);
    lcd_set_backdrop(NULL);
    lcd_setfont(FONT_SYSFIXED);
    spk_draw_full_flush();

    /* Half, as latched, double. The grid moves with it, anchored so the
     * next boundary is a whole new period away -- free here, because the
     * music is stopped and nothing is crossing one. */
    if (m.tempo_changed)
    {
        int unshifted = tempo_shift < 0 ? beat_ms << -tempo_shift
                                        : beat_ms >> tempo_shift;

        beat_ms = spike_shifted(unshifted);
        spike_anchor((long)spk_clock_ms());
    }

    return root;
}

/* The end of a run, held until the player is done looking at it. Its own
 * loop, because it has nothing in common with the game's: no grid, no
 * clock, and one thing moving. */
static void spike_result_screen(const struct spk_result *r)
{
    while (1)
    {
        long t = current_tick;
        int phase = (int)((t % SPK_RESULT_BEAT) * SPK_PHASE
                          / SPK_RESULT_BEAT);
        int move = (int)((t / SPK_RESULT_BEAT) & 3);
        int button;

        spk_draw_result(r, phase, move);

        button = get_action(CONTEXT_SPIKE, HZ / 20);

        if (button == ACTION_SPIKE_JUMP || button == ACTION_SPIKE_EXIT
            || button == ACTION_SPIKE_PAUSE)
            break;

        default_event_handler(button);
    }
}

bool spike_screen(enum spike_mode mode)
{
    struct viewport vp;
    long next_frame, rate_due, mark;
    long drawn_ticks = 0, flush_ticks = 0;
    int frame_ticks = SPK_FRAME_MIN;
    int frames = 0, overruns = 0, clean = 0, fps = 0;
    int draw_ms = 0, flush_ms = 0;
    int status;
    bool boosted = false, running, clock_lost = false;

    if (!(audio_status() & AUDIO_STATUS_PLAY))
    {
        splash(HZ * 2, "Play a track first");
        return false;
    }

    push_current_activity(ACTIVITY_SPIKE);

    /* The screen owns the whole display: the status bar has nothing to say
     * over a game, and a theme backdrop would turn clearing the field from
     * a fill into a copy of 77KB of bitmap every frame. */
    viewportmanager_theme_enable(SCREEN_MAIN, false, &vp);
    lcd_set_backdrop(NULL);
    lcd_setfont(FONT_SYSFIXED);
    lcd_clear_display();
    lcd_update();
    spike_backlight(true);

    press_total = 0;
    press_count = 0;
    press_last = 0;
    play_mode = mode;

    /* Paused counts as playing -- PLAY_PAUSED is AUDIO_STATUS_PLAY with the
     * pause bit beside it -- so the check above lets a paused player in, and
     * a paused player has no clock. Everything here is cut from the position
     * report, and a stopped report is a field that never moves and a beat
     * that is never found: the run waits for ever and looks hung.
     *
     * Started rather than refused, because "Play with Spike" is an
     * instruction. */
    spike_set_paused(false);

    /* Read once, on the way in: the file is not touched again until the run
     * ends, and never while one is running. */
    best_path[0] = '\0';
    if (mode == SPIKE_MODE_RUN)
        spk_score_run(&best_score, &best_beats);
    else
    {
        struct mp3entry *id3 = audio_current_track();

        if (id3 != NULL && id3->path != NULL)
            strlcpy(best_path, id3->path, sizeof (best_path));

        best_score = spk_score_track(best_path);
        best_beats = 0;
    }

    /* Both modes pick the music up where it is. A run is played against the
     * music that is on, not against a track from the top, and the scoring
     * already answers the objection that entering late is an easier run:
     * a death costs 1000, so nothing is won by meeting less course. Only a
     * death seeks, and only in Song. */
    spk_clock_reset();
    spike_reset_run();

    next_frame = current_tick + frame_ticks;
    rate_due = current_tick + SPK_RATE_TICKS;

    while (1)
    {
        struct spk_frame frame;
        long wait = next_frame - current_tick;
        long grid_ms;
        int button, beat;

        if (wait < 1)
            wait = 1;

        button = get_action(CONTEXT_SPIKE, wait);

        if (button == ACTION_SPIKE_EXIT)
        {
            /* The music is still playing here, so the run carries on
             * behind the question and the clock stays the run's. */
            if (mode != SPIKE_MODE_RUN || spike_confirm_exit())
                break;

            next_frame = current_tick + frame_ticks;
            continue;
        }

        if (button == ACTION_SPIKE_JUMP)
        {
            /* Timestamped against the clock rather than against the frame:
             * the loop wakes on the button, so a press is judged where it
             * fell and not where the next frame happened to be. */
            spike_press((long)spk_clock_now() - anchor_ms - offset_ms);
        }

        /* The WPS's controls, carried over: a run is the WPS with a game
         * over it, and the player should not have to leave to turn the
         * music down. The wheel is the volume while the field is moving and
         * the offset slider on the pause overlay, which is a screen that is
         * already stopped.
         *
         * Song has no skip. Skipping is safe by construction in Run -- a
         * track change is spike_listen() and is seamless -- but a track
         * Song did not play to the end is an attempt abandoned rather than
         * one finished, and there is nothing to report for it. */
        if (button == ACTION_SPIKE_UP)
            adjust_volume(1);
        else if (button == ACTION_SPIKE_DOWN)
            adjust_volume(-1);
        else if (button == ACTION_SPIKE_NEXT && mode == SPIKE_MODE_RUN)
            audio_next();
        else if (button == ACTION_SPIKE_PREV && mode == SPIKE_MODE_RUN)
        {
            /* The WPS's rule, unchanged: near the top of a track it goes
             * back one, and after that it goes back to the top. */
            if (spk_clock_ms() < DEFAULT_SKIP_THRESH)
                audio_prev();
            else
            {
                audio_pre_ff_rewind();
                audio_ff_rewind(0);
            }
        }

        if (button == ACTION_SPIKE_PAUSE || button_hold())
        {
            bool by_hold = button_hold();

            spike_set_paused(true);
            if (spike_paused(by_hold))
                break;

            next_frame = current_tick + frame_ticks;
            continue;
        }

        /* Held Menu. The music stops while it is open, and it has to: a run
         * left playing under a menu is a run whose clock has moved past
         * anything the field could be caught up to. */
        if (button == ACTION_SPIKE_OPTIONS)
        {
            spike_set_paused(true);
            if (spike_menu(fps, draw_ms, flush_ms))
                break;

            spike_set_paused(false);
            next_frame = current_tick + frame_ticks;
            continue;
        }

        default_event_handler(button);

        /* Boosted only while the field is moving. Unboosted the 5G runs at
         * 30MHz against 80 and the bus drops with it; nothing else asks for
         * the boost, since the codec raises it to refill and drops it
         * again. Tied to playback rather than to the achieved frame rate,
         * because unboosting makes frames several times slower and a
         * rate-based rule would flap every second. */
        status = audio_status();
        running = (status & AUDIO_STATUS_PLAY)
                  && !(status & AUDIO_STATUS_PAUSE);

        if (running != boosted)
        {
            cpu_boost(running);
            boosted = running;
        }

        /* Woken by a key rather than by the deadline: act on it, but do not
         * draw early. A frame drawn off the cadence is the judder the
         * deadline exists to remove. */
        if (TIME_BEFORE(current_tick, next_frame))
            continue;

        next_frame += frame_ticks;

        /* A frame that overran leaves the deadline behind. Run the next at
         * once rather than waiting a further period from here: adding a
         * whole period on top of an overrun is what turns a frame a little
         * too slow into a frame rate less than half the target. */
        if (TIME_BEFORE(next_frame, current_tick))
        {
            next_frame = current_tick;
            overruns++;
        }

        frames++;
        if (TIME_AFTER(current_tick, rate_due))
        {
            fps = frames;
            draw_ms = frames
                      ? (int)(drawn_ticks * (1000 / HZ)) / frames : 0;
            flush_ms = frames
                       ? (int)(flush_ticks * (1000 / HZ)) / frames : 0;

            /* Give the deadline a period it can keep: widened as soon as a
             * quarter of a second's frames miss it, narrowed only after
             * three clean seconds. The asymmetry is the point -- an
             * alternating period is an alternating scroll step, which is
             * the one thing an even cadence exists to prevent. */
            if (overruns * 4 > frames)
            {
                if (frame_ticks < SPK_FRAME_MAX)
                    frame_ticks++;
                clean = 0;
            }
            else if (overruns > 0)
                clean = 0;
            else if (++clean >= 3 && frame_ticks > SPK_FRAME_MIN)
            {
                frame_ticks--;
                clean = 0;
            }

            frames = 0;
            overruns = 0;
            drawn_ticks = 0;
            flush_ticks = 0;
            rate_due = current_tick + SPK_RATE_TICKS;
        }

        /* False is a seek, a skip or a track change: the grid it was cut
         * from no longer describes this audio, so the run starts again
         * rather than being carried across.
         *
         * Once per episode, not once per frame. A position key that keeps
         * moving -- which a track change can do for a while -- would
         * otherwise restart the count-in on every frame, and it would sit
         * on four for as long as the key was unsettled. */
        if (!spk_clock_tick())
        {
            if (!clock_lost)
            {
                clock_lost = true;

                /* Nothing seeks in Song, so this is the track ending under
                 * the player and the attempt is finished -- won or not,
                 * there is no more of it.
                 *
                 * Only once there is a run to end, though. Song is entered
                 * on a track that has just been started, and the position
                 * report takes a moment to leave whatever was playing
                 * before: the first thing the clock sees is a jump
                 * backwards into the new track, which is the same signal a
                 * track change gives. Before the tempo latches nothing has
                 * been attempted, so that is the audio settling into the
                 * track rather than leaving it. */
                if (play_mode == SPIKE_MODE_SONG
                    && run_state != SPK_RUN_COUNT
                    && spike_track_changed())
                    run_state = SPK_RUN_OVER;

                /* Not a new game. The world keeps its cell, its ground and
                 * its walk; only the tempo has to be found again. */
                else if (run_state == SPK_RUN_PLAY
                         || run_state == SPK_RUN_COUNT)
                    spike_listen(world.beat);
                else
                    spike_reset_run();
            }
        }
        else
            clock_lost = false;

        /* Run ends with the playlist. Pausing keeps AUDIO_STATUS_PLAY, so
         * this is playback stopping and nothing else.
         *
         * Held for a moment rather than acted on at once: a single frame
         * that catches playback between tracks would otherwise end a run,
         * and ending one is not a thing to get wrong on one reading. */
        if (audio_status() & AUDIO_STATUS_PLAY)
            stopped_at = 0;
        else if (stopped_at == 0)
            stopped_at = current_tick;
        else if (TIME_AFTER(current_tick, stopped_at + SPK_STOPPED_TICKS))
            run_state = SPK_RUN_OVER;

        if (run_state == SPK_RUN_OVER)
            break;

        /* Only while the count-in is waiting on a lock. A track change
         * restarts both, through spike_reset_run(). */
        if (run_state == SPK_RUN_COUNT)
        {
            beat_analyse_poll();
            spk_bar_listen();
        }

        grid_ms = spike_now();

        last_grid = grid_ms;
        beat = (int)(grid_ms / beat_ms);

        /* Only ever forward, and only ever one beat at a time. A grid that
         * is further behind than a slow frame can explain has been moved
         * under the run, so the run is restarted rather than fast-forwarded
         * through boundaries the player never saw. */
        if (beat - cur_beat > SPK_JUMP_BEATS)
        {
            spike_reset_run();
            grid_ms = spike_now();
            beat = 0;
        }

        /* And backwards is not a beat that can be shown at all.
         *
         * The clock can be dragged back -- a track still draining reports a
         * position from the old one, a seek lands short -- and a grid behind
         * its own beat counter stops crossing boundaries *for ever*: the
         * loop only ever counts up to it. The field then freezes while the
         * sub-beat phase carries on, so the body animates on the spot and
         * nothing else moves, which is not a state anything recovers from.
         *
         * Re-anchored rather than restarted, because nothing the player can
         * see needs to change: the next boundary is put a beat away and the
         * counter carries straight on. */
        if (beat < cur_beat)
        {
            spike_anchor((long)spk_clock_ms() + beat_ms);
            grid_ms = spike_now();
            beat = (int)(grid_ms / beat_ms);
        }

        while (beat > cur_beat)
        {
            cur_beat++;
            spike_boundary(grid_ms);
        }

        /* The moment the bodies meet. Asked every frame rather than at the
         * boundary, so what the player sees when the death begins is the
         * two of them touching -- which is the whole of what happened. */
        if (run_state == SPK_RUN_PLAY
            && (grid_ms % beat_ms) * SPK_PHASE / beat_ms >= SPK_TOUCH_PHASE)
        {
            enum spk_outcome ahead = spk_state_peek(&world);

            if (ahead == SPK_HIT || ahead == SPK_CRUSHED
                || ahead == SPK_BONKED)
                spike_die(SPK_DEATH_OUCH, grid_ms,
                             spk_ease((int)((grid_ms % beat_ms) * SPK_PHASE
                                           / beat_ms)));
        }

        /* Bowing out. The course stops offering obstacles while there is
         * still a phrase or two to cross, and that is the whole of it: the
         * triangle walks over the join into the next track's silence and out
         * the other side of it, because a run that stops and starts again at
         * every track boundary is not one run. */
        if (run_state == SPK_RUN_PLAY)
        {
            /* The cell the music runs out on. Asked of the assembler rather
             * than of the player: it lays a pattern or two in front of the
             * field, so a test at the player's own position stops obstacles
             * long after the last of them has already been placed. */
            long ends_at = world.beat
                           + (long)(spk_clock_left_ms()
                                    / (unsigned long)beat_ms);

            spk_gen_set_flat(spk_gen_next_cell() + SPK_OUTRO_SLACK >= ends_at);
        }

        /* And a beat after it began, whenever that was, the run moves on
         * without the player in it -- or, in Song, without the attempt. */
        if (run_state == SPK_RUN_DEAD && grid_ms - death_at >= beat_ms)
        {
            /* Song has one life a track. Broken off here rather than at the
             * top of the next frame so that the last thing drawn is the end
             * of the death, and the results screen replaces it directly. */
            if (play_mode == SPIKE_MODE_SONG)
            {
                run_state = SPK_RUN_OVER;
                break;
            }
            else
            {
                run_state = SPK_RUN_SKIP;

                /* Before the respawn is hunted for, not after. A switch
                 * thrown on the line that has just ended goes off with it,
                 * and a cell whose floor was only there because of it is
                 * not ground to come back to -- picked while the switch
                 * still counted, the respawn lands the player in a hole. */
                spk_world_forget();
                respawn_cell = spk_world_respawn(world.beat + SPK_SKIP_BEATS);
                world.beat++;
            }
        }

        spike_fill_frame(&frame, grid_ms);

        mark = current_tick;
        spk_draw_frame(&frame);
        drawn_ticks += current_tick - mark;

        mark = current_tick;
        spk_draw_flush();
        flush_ticks += current_tick - mark;
    }

    beat_analyse_stop();

    /* What the run was worth, and whether it is kept.
     *
     * Run keeps whatever it reached, because leaving is one of the two ways
     * a run ends and a good one should not be lost by walking away from it.
     * Song keeps an attempt that ended on its own -- a death or the end of
     * the track, which are its only two endings. What it does not keep is
     * one abandoned with MENU: a score banked by quitting at a good moment
     * is not an attempt at anything.
     *
     * The screen is shown on the same terms. A player pressing MENU is on
     * their way somewhere and a results screen in the way is not a reward,
     * which is what the crown on the field is for -- said while it is
     * happening. */
    /* A run that never found a tempo never began, and has nothing to
     * report: no score, no distance, and a results screen over it would be
     * a screen about nothing. */
    if (run_began && (run_state == SPK_RUN_OVER || mode == SPIKE_MODE_RUN))
    {
        struct spk_result r;

        r.score = score;
        r.best = best_score;
        r.beats = run_beats;
        r.run = mode == SPIKE_MODE_RUN;
        r.crowned = mode == SPIKE_MODE_RUN
                    ? spk_score_put_run(score, run_beats)
                    : spk_score_put_track(best_path, score);

        if (run_state == SPK_RUN_OVER)
            spike_result_screen(&r);
    }

    if (boosted)
        cpu_boost(false);
    spike_backlight(false);
    lcd_setfont(FONT_UI);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
    pop_current_activity();

    return false;
}

bool spike_run_screen(void)
{
    return spike_screen(SPIKE_MODE_RUN);
}

bool spike_song_screen(void)
{
    return spike_screen(SPIKE_MODE_SONG);
}
