// The movement rules, and what the player finds where they land.
//
// The course comes from gen: this file asks it for a cell and never knows
// which phrase the cell came from. Nothing here knows about pixels,
// milliseconds or input -- one call crosses one beat boundary.
//
// The player's state is a record (see newState): beat, the cell occupied;
// level, the surface under it or -1 airborne; motion, what covers this beat;
// from and to, the levels it starts and ends at (to -1 for nothing to land
// on); apex, the height an airborne move reaches; and the marks the boundary
// just crossed left: landed, got (and gotLevel), threw, stomped, sprung.
//
// Parts: reading a cell; what has been taken; the player.

// Surfaces sit at four levels; 0 is the ground
const LEVELS = 4;

// A press's arc rises ARC_UP levels but lands at most CLIMB_UP above where
// it left: climbing one level at a time gives each level its turn
const ARC_UP = 2, CLIMB_UP = 1;

// A rising block fills the ground when down and this level when up
const BLOCK_LEVEL = 2;

// What covers the beat just begun; each covers exactly one cell
const WALK = 0, ARC_RISE = 1, ARC_FALL = 2, ARC_SPRING = 3;

// How a boundary ended
const OUT_OK = 0, OUT_FELL = 1, OUT_HIT = 2, OUT_CRUSHED = 3, OUT_BONKED = 4;

// ---- reading a cell --------------------------------------------------------

function mask(c) {
    return gen.cell(c) & gen.C_MASK;
}

// A cell's surfaces with its switch on or off
function liveWith(e, on) {
    var m = e & gen.C_MASK;
    var mode = (e & gen.C_SWM) >> gen.SWM_SH;
    if (mode == gen.SW_PLAT) return on ? m : m & 1;
    if (mode == gen.SW_GROUND) return on ? m : m & ~1;
    if (mode == gen.SW_SWAP) return on ? m & 1 : m & ~1;
    return m;
}

// What is there this moment
function live(c) {
    return liveWith(gen.cell(c), switchOn(c));
}

// What throwing the switch would add, which the drawing dashes in
function promised(c) {
    if (switchOn(c))
        return 0;
    var e = gen.cell(c);
    return liveWith(e, true) & ~liveWith(e, false);
}

function blocks(c) {
    return (gen.cell(c) & gen.C_BLF) != 0;
}

// On the bar's four: bit n is the state on the pattern's own nth beat
function blockUp(c, b) {
    var pattern = (gen.cell(c) >> gen.BL_SH) & 15;
    return ((pattern >> ((b - gen.bar()) & 3)) & 1) != 0;
}

function switched(c) {
    return (gen.cell(c) & gen.C_SWM) != 0;
}

function flew(c) {
    return (gen.cell(c) & gen.C_FLY) != 0;
}

// The switch's level, or -1 for none
function hasSwitch(c) {
    var e = gen.cell(c);
    return (e & gen.C_SWF) ? (e >> gen.SW_SH) & 3 : -1;
}

// ---- what has been taken ---------------------------------------------------

// A small ring storing the cell each slot is about, so a stale mark from a
// lap ago cannot be read
const MARKS = 64;
const M_DIAMOND = 1, M_CREATURE = 2, M_SWITCH = 4;

var markCell = new Int32Array(MARKS);
var markBits = new Int32Array(MARKS);

// The phrase whose switch is thrown; one at a time
var switchPattern = -1;

function slot(c) {
    var i = c % MARKS;
    return i < 0 ? i + MARKS : i;
}

function marked(c, what) {
    var i = slot(c);
    return markCell[i] == c && (markBits[i] & what) != 0;
}

function mark(c, what) {
    var i = slot(c);
    if (markCell[i] != c) {
        markCell[i] = c;
        markBits[i] = 0;
    }
    markBits[i] |= what;
}

function forget() {
    fill(markCell, -1);
    fill(markBits, 0);
    switchPattern = -1;
}

// Each of these is the thing's level, or -1 when there is none (or it has
// been taken)
function diamond(c) {
    var e = gen.cell(c);
    if (!(e & gen.C_DIF) || marked(c, M_DIAMOND))
        return -1;
    return (e >> gen.DI_SH) & 3;
}

function creature(c) {
    var e = gen.cell(c);
    if (!(e & gen.C_CRF) || marked(c, M_CREATURE))
        return -1;
    return (e >> gen.CR_SH) & 3;
}

// A flyer is gone once stomped, sharing the creature's mark
function flyer(c) {
    var e = gen.cell(c);
    if (!(e & gen.C_FLY) || marked(c, M_CREATURE))
        return -1;
    return (e >> gen.FL_SH) & 3;
}

// Which kind of creature: asked only where there is one
function spiked(c) {
    return (gen.cell(c) & gen.C_SPK) != 0;
}

function spring(c) {
    return (gen.cell(c) & gen.C_SPR) != 0;
}

function switchAt(c) {
    var l = hasSwitch(c);
    return l >= 0 && !marked(c, M_SWITCH) ? l : -1;
}

function switchOn(c) {
    return switchPattern >= 0 && gen.patternStart(c) == switchPattern;
}

// The highest surface at or below maxLevel, or -1
function surface(c, maxLevel) {
    var m = live(c), level = maxLevel;
    if (level > LEVELS - 1)
        level = LEVELS - 1;
    for (; level >= 0; level--)
        if (m & (1 << level))
            return level;
    return -1;
}

// What a cell offers a respawn: its own phrase's pad counted as thrown
function respawnLive(c, phrase) {
    return liveWith(gen.cell(c), gen.patternStart(c) == phrase || switchOn(c));
}

// Throw the pad in that cell's phrase, as landing on it would
function throwPad(c) {
    var start = gen.patternStart(c);
    for (var i = 0; i < gen.PAT_MAX; i++) {
        if (gen.patternStart(start + i) != start)
            break;
        if (switchAt(start + i) >= 0) {
            mark(start + i, M_SWITCH);
            switchPattern = start;
            return;
        }
    }
}

// The first cell at or after from with somewhere to stand and two clear
// beats of walking after it; respawnLevel is the lowest surface it has
var respawnLevel = 0;

function respawn(from) {
    for (var c = from; c < from + 64; c++) {
        var phrase = gen.patternStart(c);
        var m = respawnLive(c, phrase), at, i;
        for (at = 0; at < LEVELS; at++)
            if (m & (1 << at))
                break;
        if (at >= LEVELS)
            continue;
        var stood = at;
        for (i = 0; i < 3; i++) {
            var lv = respawnLive(c + i, phrase), next = -1;
            for (var l = stood; l >= 0; l--)
                if (lv & (1 << l)) {
                    next = l;
                    break;
                }
            if (next < 0 || creature(c + i) >= 0 || flyer(c + i) >= 0 || blocks(c + i))
                break;
            stood = next;
        }
        if (i == 3) {
            respawnLevel = at;
            return c;
        }
    }
    respawnLevel = 0;
    return from;
}

// ---- the player ------------------------------------------------------------

function newState() {
    return { beat: 0, level: 0, motion: WALK, from: 0, to: 0, apex: 0,
             landed: false, got: false, threw: false, gotLevel: 0,
             stomped: false, sprung: false };
}

function copyState(d, s) {
    d.beat = s.beat; d.level = s.level; d.motion = s.motion;
    d.from = s.from; d.to = s.to; d.apex = s.apex;
    d.landed = s.landed; d.got = s.got; d.threw = s.threw;
    d.gotLevel = s.gotLevel; d.stomped = s.stomped; d.sprung = s.sprung;
}

// The highest thing a jump could come down on: the surface, or a flyer
// riding higher over it
function landing(c, ceiling) {
    var at = surface(c, ceiling);
    if (ceiling > LEVELS - 1)
        ceiling = LEVELS - 1;
    var cl = flyer(c);
    if (cl >= 0 && cl <= ceiling && cl > at)
        return cl;
    return at;
}

// Choose the move that covers the beat now beginning
function beginMove(st) {
    st.from = st.level;
    if (st.sprung) {
        st.sprung = false;
        st.motion = ARC_SPRING;
        st.apex = LEVELS - 1;
        st.to = landing(st.beat + 2, st.apex);
        return;
    }
    st.motion = WALK;
    st.to = surface(st.beat + 1, st.level);
}

// What the player finds where it has just arrived
function arrive(st, level, fromAbove, over) {
    var cl;
    if (!over && creature(st.beat) == level) {
        if (!fromAbove || spiked(st.beat))
            return OUT_HIT;
        mark(st.beat, M_CREATURE);
        st.stomped = true;
    }
    if (!over && flyer(st.beat) == level) {
        if (!fromAbove)
            return OUT_HIT;
        mark(st.beat, M_CREATURE);
        st.stomped = true;
    }
    if (fromAbove && !over && switchAt(st.beat) == level) {
        mark(st.beat, M_SWITCH);
        switchPattern = gen.patternStart(st.beat);
        st.threw = true;
    }
    cl = diamond(st.beat);
    if (cl >= 0 && cl == level) {
        mark(st.beat, M_DIAMOND);
        st.got = true;
        st.gotLevel = cl;
    }
    if (fromAbove && !over && level == 0 && spring(st.beat))
        st.sprung = true;
    return OUT_OK;
}

function place(st, c, level) {
    st.beat = c;
    st.level = level;
    st.landed = false;
    st.got = false;
    st.gotLevel = 0;
    st.stomped = false;
    st.threw = false;
    st.sprung = false;
    st.apex = 0;
    beginMove(st);
}

// Stand somewhere, forgetting what the line before did
function startAt(st, c, level) {
    forget();
    place(st, c, level);
}

function start(st, c) {
    startAt(st, c, 0);
}

// Stand where a respawn found, throwing the pad there; the caller has
// already called forget()
function respawnAt(st, c, level) {
    throwPad(c);
    place(st, c, level);
}

// What the move under way will end in, without ending it
function peek(st) {
    if ((st.motion == WALK || st.motion == ARC_FALL) && st.to < 0)
        return OUT_FELL;
    if (st.motion == WALK || st.motion == ARC_FALL) {
        var above = st.motion == ARC_FALL || st.from > st.to;
        var cl = creature(st.beat + 1);
        if (cl >= 0 && cl == st.to && (!above || spiked(st.beat + 1)))
            return OUT_HIT;
        cl = flyer(st.beat + 1);
        if (cl >= 0 && cl == st.to && !above)
            return OUT_HIT;
    }
    if (blocks(st.beat + 1)) {
        var fills = blockUp(st.beat + 1, st.beat + 1) ? BLOCK_LEVEL : 0;
        if (st.motion == ARC_RISE || st.motion == ARC_SPRING) {
            if (st.apex == fills)
                return OUT_BONKED;
        } else if (st.to == fills)
            return OUT_CRUSHED;
    }
    return OUT_OK;
}

// Cross a beat boundary: anything but OUT_OK leaves the state where it was
function advance(st) {
    var out = peek(st);
    if (out != OUT_OK)
        return out;
    st.beat++;
    endBeat(st);
    if (st.motion == ARC_RISE || st.motion == ARC_SPRING) {
        st.level = -1;
        st.motion = ARC_FALL;
        out = arrive(st, st.apex, false, true);
    } else if (st.motion == ARC_FALL) {
        st.level = st.to;
        st.landed = true;
        out = arrive(st, st.level, true, false);
        beginMove(st);
    } else {
        st.landed = false;
        out = arrive(st, st.to, st.from > st.to, false);
        st.level = st.to;
        beginMove(st);
    }
    return out;
}

// Whether the move the player did not make would have ended the line:
// what an obstacle is. Asked before the boundary; nothing changes
var other = newState();

function avoided(st) {
    copyState(other, st);
    if (st.motion == WALK) {
        jump(other);
        if (other.to < 0)
            return true;
    } else if (st.motion == ARC_RISE)
        beginMove(other);
    else
        return false;
    return peek(other) != OUT_OK;
}

function endBeat(st) {
    st.got = false;
    st.stomped = false;
    st.threw = false;
}

// Commit a jump to the boundary just crossed; false while one is under way
function jump(st) {
    if (st.motion != WALK)
        return false;
    st.motion = ARC_RISE;
    st.from = st.level;
    st.apex = st.level + ARC_UP;
    st.to = landing(st.beat + 2, st.from + CLIMB_UP);
    return true;
}
