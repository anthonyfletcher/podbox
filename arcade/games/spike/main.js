// Spike: a triangle that steps and jumps on the beat of whatever is
// playing, over a course laid a phrase at a time ahead of it.
//
// The grid is cut from the music's clock at the tempo the beat tracker
// finds, latched once at the end of the wait and never revisited: the
// player's sense of the beat is the game's whole asset, and a grid that
// moved under it would be worse than one slightly wrong. A cell is a beat.
//
// Parts: the tempo; the grid; input; the run; the screen.

// A press inside this of a boundary is that beat's; anything later belongs
// to the next one
const WINDOW_MS = 110;

// How far ahead of a death the run looks for somewhere to come back
const DROP_BEATS = 3;

// Slack on the end of the outro test: the phrase about to be laid, and a
// bar to walk it off in
const OUTRO_SLACK = 16 + 4;

// Beats of waiting before the analyser is given the track again
const RELISTEN_BEATS = 24;

// Beats the grid may be behind before it is skipped rather than walked
const JUMP_BEATS = 4;

// Ticks with no track before the run is called over
const STOPPED_TICKS = 16;

// What things are worth: what the player did, not the metronome ticking
const PTS_AVOID = 50, PTS_DIAMOND = 100, PTS_STOMP = 500, PTS_PRESS = 25;
const PTS_DEATH = 1000, PTS_PHRASE = 500;
const MULT_MAX = 8;

// A press this close to the beat builds the combo
const PRECISE_MS = 45;

// Where in a beat the body's leading edge meets the next cell
const TOUCH_PHASE = 37;

// What the grid aims for, and the range an octave of a real tempo may land
// in; under FAST takes the beat above
const BEAT_TARGET = 500, BEAT_FAST = 420, BEAT_MIN = 330, BEAT_MAX = 840;

// The run under way, in saved numbers -- cheap to set every beat, and
// written out whenever the game is paused or ends, Quit included -- so a
// run left by Quit is kept when the cart next starts. 0 is 1 while a run is
// under way; 1 its score, 2 its beats, 3 its seconds, 4 its mean tempo in
// tenths of a BPM. Its tracks are in the store as "run", written as each
// one starts, while the beat is being found and there is no play to upset.
const D_LIVE = 0, D_SCORE = 1, D_BEATS = 2, D_SECS = 3, D_BPM10 = 4;

// Where the run is
const RUN_COUNT = 0, RUN_PLAY = 1, RUN_DEAD = 2, RUN_SKIP = 3, RUN_OVER = 4;

// How a run ended a line: off an edge, short of a landing, or into something
const DEATH_LEDGE = 0, DEATH_AIR = 1, DEATH_OUCH = 2;

var bs = new Int32Array(6);         // beat.state
var mood = new Int32Array(3);       // beat.mood

var beatMs = BEAT_TARGET;
var tempoBpm = 0;
var anchor = 0;                     // grid time zero, in the music's clock
var curBeat = 0;
var runState = RUN_COUNT;
var runBeats = 0;
var runBegan = false;
var stoppedFor = 0;
var listened = 0;
var lastGrid = 0;
var deathAt = 0;
var respawnCell = 0, respawnLevel = 0;
var pending = -1;                   // a press that belongs to the next beat
var combo = 0;
var phraseAt = 0, phraseGot = 0;
var best = 0;                       // the record's score, or 0
var bpmTotal = 0, bpmTracks = 0, bpmCounted = false;
var played = [];                    // the tracks the run was played over: { t, g }
var viewing = false;                // the record is on screen, over the run
var startedAt = 0;

var st;                             // the player, a world state

// The frame, as field.js reads it
var phase = 0, nowMs = 0, strong = true, frozen = false;
var deathPhase = -1, deathScroll = 0, deathKind = DEATH_LEDGE;
var skipping = false, dropCells = -1, dropGrow = 0, dropLevel = 0;
var points = 0, mult = 1, waiting = true, crowned = false;
var silent = false;                 // no music moving: nothing to play to
var crossed = false;                // this tick crossed a beat
var volume = -1;                    // the volume, 0-100, while it is shown; else -1
var volumeUntil = 0;                // ...until then, in t()

// How long the volume holds the caption's room after the wheel turns: long
// enough to read after a run of clicks, short enough that a brush of the
// wheel does not hide the track for the rest of the phrase
const VOLUME_SHOWN = 1.5;

// ---- the tempo ---------------------------------------------------------------

// Floor division, for a grid time that can be behind zero
function fdiv(a, b) {
    return a >= 0 ? a / b : -((b - 1 - a) / b);
}

// The track's beat times a power of two, nearest the target, and the beat
// above where that is quicker than can be read
function octave(ms) {
    var shifts = [-2, -1, 0, 1], best = BEAT_TARGET, err = -1;
    if (ms <= 0)
        return BEAT_TARGET;
    for (var i = 0; i < 4; i++) {
        var m = shifts[i] < 0 ? ms >> -shifts[i] : ms << shifts[i];
        var e = abs(m - BEAT_TARGET);
        if (err < 0 || e < err) {
            err = e;
            best = m;
        }
    }
    if (best >= BEAT_MIN && best < BEAT_FAST)
        best *= 2;
    return best;
}

// The octave the owner asked for on top
function shifted(ms) {
    var k = opt.get(OPT.tempo);
    return k == 0 ? ms * 2 : k == 2 ? ms / 2 : ms;
}

// Re-anchor so the boundary after this one falls at 'at'
function anchorAt(at) {
    anchor = at - (curBeat + 1) * beatMs;
}

// Take the tempo once and keep it; false where it is not usable yet
function latch() {
    beat.state(bs);
    beatMs = octave(bs[2]);
    tempoBpm = bs[1];
    if (beatMs < BEAT_MIN || beatMs > BEAT_MAX || tempoBpm <= 0) {
        tempoBpm = 0;
        return false;
    }
    beatMs = shifted(beatMs);
    gen.setTempo(beatMs);
    if (!bpmCounted) {
        bpmTotal += tempoBpm;
        bpmTracks++;
        bpmCounted = true;
    }
    beat.listen(false);
    var now = beat.clock(), next = beat.next(now, 0);
    anchorAt(next >= 0 ? next : now + beatMs);
    return true;
}

// Which residue of four the music's downbeat falls on, in the cells a world
// started at 'cell' now will count: phrases then begin where bars do
function barOf(cell) {
    if (beat.downbeat() < 0)
        return 0;
    var now = beat.clock();
    for (var k = 0; k < 4; k++) {
        var at = beat.next(now, k);
        if (at >= 0 && beat.inbar() == 0) {
            var g = fdiv(at - anchor + beatMs / 2, beatMs);
            return (cell + g - curBeat) & 3;
        }
    }
    return 0;
}

// ---- the grid ----------------------------------------------------------------

// Back to listening, keeping everything the player can see: the cell, the
// ground, the walk. A track change is not a new game.
function listen(cell) {
    tempoBpm = 0;
    listened = 0;
    pending = -1;
    combo = 0;
    beat.listen(true);
    gen.setBar(0);
    anchor = beat.clock() - lastGrid;
    gen.reset(cell);
    gen.setRun(runBeats, cell);
    gen.setFlat(true);
    world.start(st, cell);
    phraseAt = gen.patternStart(cell);
    phraseGot = -1;
    runState = RUN_COUNT;
}

// A new game
function resetRun() {
    beatMs = shifted(BEAT_TARGET);
    field.reset();
    anchor = beat.clock();
    lastGrid = 0;
    curBeat = 0;
    deathKind = DEATH_LEDGE;
    respawnCell = 0;
    phraseGot = 0;
    points = 0;
    runBeats = 0;
    runBegan = false;
    stoppedFor = 0;
    bpmTotal = 0;
    bpmTracks = 0;
    bpmCounted = false;
    played = [nowPlaying()];
    store.write("run", played);
    startedAt = t();
    listen(fdiv(beat.clock(), beatMs));
}

// ---- input -------------------------------------------------------------------

// A press, in grid time: the nearest boundary inside its window, else the
// next one, which is buffered
function press(at) {
    if (at < 0)
        return;
    var b = at / beatMs, frac = at % beatMs, target, err;
    if (frac <= WINDOW_MS) {
        target = b;
        err = frac;
    } else {
        target = b + 1;
        err = frac - beatMs;
    }
    if (runState != RUN_PLAY)
        return;
    if (err <= PRECISE_MS && err >= -PRECISE_MS) {
        points += PTS_PRESS;
        combo++;
    }
    if (target == curBeat)
        world.jump(st);
    else if (target == curBeat + 1)
        pending = target;
}

// ---- the run -----------------------------------------------------------------

function multiplier() {
    var m = 1 + (combo >> 3);
    return m > MULT_MAX ? MULT_MAX : m;
}

function die(kind, at, scroll) {
    runState = RUN_DEAD;
    deathKind = kind;
    deathAt = at;
    deathScroll = scroll;
    pending = -1;
    world.endBeat(st);
    combo = 0;
    points -= PTS_DEATH;
    if (points < 0)
        points = 0;
    phraseGot = -1;
}

// Cross one beat boundary
function boundary(grid) {
    caption.stepOn();
    beat.mood(mood);
    gen.setMood(mood[0], mood[1], mood[2]);

    if (runState == RUN_COUNT) {
        world.advance(st);
        if (beat.state(bs) && latch()) {
            var c = fdiv(beat.clock(), beatMs);
            gen.setBar(barOf(c));
            gen.reset(c);
            gen.setRun(runBeats, c);
            gen.setFlat(true);
            world.start(st, c);
            // one more phrase of flat ground, so the course scrolls in
            gen.cell(gen.nextCell());
            gen.setFlat(false);
            phraseAt = gen.patternStart(st.beat);
            phraseGot = -1;
            runBegan = true;
            noteRun();
            runState = RUN_PLAY;
            return;
        }
        if (++listened >= RELISTEN_BEATS) {
            listened = 0;
            beat.listen(false);
            beat.listen(true);
        }
        return;
    }

    if (runState == RUN_PLAY) {
        var avoided = world.avoided(st);
        var out = world.advance(st);
        runBeats++;
        noteRun();
        gen.setRun(runBeats, st.beat);
        if (out == world.OUT_HIT || out == world.OUT_CRUSHED || out == world.OUT_BONKED) {
            die(DEATH_OUCH, grid, pose.PHASE);
            return;
        }
        if (out == world.OUT_FELL) {
            die(st.motion == world.WALK ? DEATH_LEDGE : DEATH_AIR, grid, pose.PHASE);
            return;
        }
        if (gen.patternStart(st.beat) != phraseAt) {
            var had = gen.patternDiamonds(phraseAt);
            if (had > 0 && phraseGot >= had)
                points += PTS_PHRASE * multiplier();
            phraseGot = 0;
            phraseAt = gen.patternStart(st.beat);
        }
        var m = multiplier();
        if (avoided)
            points += PTS_AVOID * m;
        if (st.got) {
            points += PTS_DIAMOND * m;
            phraseGot++;
        }
        if (st.stomped)
            points += PTS_STOMP * m;
        if (pending == curBeat) {
            world.jump(st);
            pending = -1;
        }
        return;
    }

    if (runState == RUN_SKIP) {
        st.beat++;
        if (st.beat >= respawnCell) {
            world.respawnAt(st, st.beat, respawnLevel);
            phraseAt = gen.patternStart(st.beat);
            phraseGot = -1;
            runState = RUN_PLAY;
        }
    }
}

// The track playing, as the run remembers it: its title and its genre
function nowPlaying() {
    var id = playback.track();
    return { t: playback.title(), g: id >= 0 ? tracks.field(id, TRACK.GENRE) || "" : "" };
}

function bpm10() {
    return bpmTracks ? bpmTotal * 10 / bpmTracks : 0;
}

// The run so far, in the saved numbers
function noteRun() {
    dset(D_LIVE, 1);
    dset(D_SCORE, points);
    dset(D_BEATS, runBeats);
    dset(D_SECS, flr(t() - startedAt));
    dset(D_BPM10, bpm10());
}

// The music has stopped: the run is over, and what it reached is kept. A
// run that never found a tempo never began, and waits for music again.
function finish() {
    if (!runBegan) {
        stoppedFor = 0;
        return;
    }
    runState = RUN_OVER;
    beat.listen(false);
    dset(D_LIVE, 0);
    results.ended({ score: points, beats: runBeats, secs: flr(t() - startedAt),
                    bpm10: bpm10(), tracks: played });
    bestItem();
}

// A run the last session left under way, by Quit or by the player being
// switched off: kept as one that ended there
function keepLeftRun() {
    if (!dget(D_LIVE))
        return;
    dset(D_LIVE, 0);
    var tracks = store.read("run");
    if (dget(D_BEATS) > 0)
        results.keep({ score: dget(D_SCORE), beats: dget(D_BEATS), secs: dget(D_SECS),
                       bpm10: dget(D_BPM10), tracks: tracks || [] });
}

// Best run in the pause menu, once there is one
function bestItem() {
    if (results.record() != null)
        menuitem(1, "Best run", bestRun);
}

// The pause menu's Best run: the record over the run, until Select
function bestRun() {
    viewing = results.showRecord();
}

function bestOf() {
    var r = results.record();
    return r != null ? r.score : 0;
}

// ---- the screen --------------------------------------------------------------

function init() {
    gen.load(data.read(DATA.phrases).phrases);
    pose.load(data.read(DATA.poses));
    st = world.newState();
    field.colours();
    caption.update();
    keepLeftRun();
    best = bestOf();
    bestItem();
    resetRun();
}

// The music's own buttons, as on the player's Now Playing: Prev goes back a
// track in its first seconds and to the start of this one after them, Next
// on a track, and Play pauses and plays -- as does Select while paused
const BACK_MS = 3000;

function musicButtons() {
    if (btnp(1))
        playback.next();
    else if (btnp(0)) {
        if (playback.elapsed() < BACK_MS)
            playback.prev();
        else
            playback.seek(0);
    }
    if (btnp(3) || (silent && !viewing && runState != RUN_OVER && playback.track() >= 0 && btnp(4)))
        playback.toggle();
}

function update() {
    musicButtons();
    if (viewing) {
        viewing = results.update();
        return;
    }
    if (runState == RUN_OVER) {
        if (!results.update()) {
            best = bestOf();
            resetRun();
        }
        return;
    }
    if (playback.changed()) {
        field.colours();
        caption.update();
        bpmCounted = false;
        played.push(nowPlaying());
        if (runBegan)
            store.write("run", played);
    }

    // the music moved under the run: a seek, a skip, a new track
    if (beat.jumped()) {
        if (runState == RUN_PLAY || runState == RUN_COUNT)
            listen(st.beat);
        else
            resetRun();
    }

    // stopped, not paused: the run is over
    if (playback.track() < 0) {
        if (++stoppedFor >= STOPPED_TICKS) {
            finish();
            return;
        }
    } else
        stoppedFor = 0;
    // the wheel is the volume, a step a click, as on the player's screens
    var w = wheel();
    if (w != 0) {
        volume = playback.adjust(w);
        volumeUntil = t() + VOLUME_SHOWN;
    } else if (volume >= 0 && t() >= volumeUntil)
        volume = -1;

    silent = !playback.playing();
    if (silent) {
        if (!runBegan)
            title.update();
        return;
    }

    var grid = beat.clock() - anchor;
    var b = fdiv(grid, beatMs);

    // a gap the grid cannot have walked -- the game was paused under
    // playing music -- is stepped over a whole number of beats, so the
    // field picks up where it was and stays on the beat
    if (b - curBeat > JUMP_BEATS) {
        anchor += (b - curBeat) * beatMs;
        grid = beat.clock() - anchor;
        b = curBeat;
    }
    if (b < curBeat) {
        anchorAt(beat.clock() + beatMs);
        grid = beat.clock() - anchor;
        b = fdiv(grid, beatMs);
    }
    lastGrid = grid;

    var p = beat.press(4);
    if (p >= 0)
        press(p - anchor);

    crossed = b > curBeat;
    while (b > curBeat) {
        curBeat++;
        boundary(grid);
    }
    // the next phrase laid between beats rather than on one, and not on the
    // frame the band is drawn again: the field shows eight cells ahead, a
    // phrase is at most sixteen
    if (!crossed && !field.bandChanged() && runState == RUN_PLAY)
        gen.layAhead(st.beat, 8 + 16 + 4);

    var sub = grid - fdiv(grid, beatMs) * beatMs;

    // the moment the bodies meet, part-way through the beat
    if (runState == RUN_PLAY && (sub * pose.PHASE) / beatMs >= TOUCH_PHASE) {
        var ahead = world.peek(st);
        if (ahead == world.OUT_HIT || ahead == world.OUT_CRUSHED || ahead == world.OUT_BONKED)
            die(DEATH_OUCH, grid, pose.ease((sub * pose.PHASE) / beatMs));
    }

    // bowing out: no more obstacles while the last phrases are crossed
    if (runState == RUN_PLAY) {
        var endsAt = st.beat + beat.left() / beatMs;
        gen.setFlat(gen.nextCell() + OUTRO_SLACK >= endsAt);
    }

    // a beat after a death, the hunt for somewhere to come back
    if (runState == RUN_DEAD && grid - deathAt >= beatMs) {
        runState = RUN_SKIP;
        world.forget();
        st.beat++;
        respawnCell = world.respawn(st.beat + DROP_BEATS);
        respawnLevel = world.respawnLevel;
    }
}

// The frame's numbers, for field.js
function fillFrame() {
    var grid = lastGrid;
    var sub = grid - fdiv(grid, beatMs) * beatMs;
    phase = (sub * pose.PHASE) / beatMs;
    dropCells = -1;
    dropGrow = 0;
    dropLevel = 0;
    if (runState == RUN_SKIP) {
        var togo = respawnCell - st.beat;
        if (togo > 0 && togo <= 2) {
            dropCells = togo;
            dropLevel = respawnLevel;
            dropGrow = togo == 2 ? phase : pose.PHASE;
        }
    }
    nowMs = grid > 0 ? grid : 0;
    strong = (curBeat & 1) == 0;
    skipping = runState == RUN_SKIP;
    mult = multiplier();
    crowned = best > 0 && points > best;
    waiting = runState == RUN_COUNT;
    if (runState == RUN_DEAD) {
        var age = ((grid - deathAt) * pose.PHASE) / beatMs;
        deathPhase = age < 0 ? 0 : age > pose.PHASE - 1 ? pose.PHASE - 1 : age;
    } else
        deathPhase = -1;
    frozen = deathPhase >= 0;
}

function draw() {
    if (silent && !runBegan && !viewing && runState != RUN_OVER) {
        title.draw();
        return;
    }
    if (viewing || runState == RUN_OVER) {
        results.draw();
        return;
    }
    fillFrame();
    field.draw();
}
