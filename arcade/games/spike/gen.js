// The phrase library and the assembler that lays phrases end to end.
//
// The unit of the course is a phrase -- a hand-written run of four to
// sixteen cells with its own entry and exit level, in data/phrases.json.
// Nothing is made a cell at a time: every phrase can be crossed on its own,
// and joining one's exit to the next one's entry keeps that true.
//
// Two placement rules hold the rest up: a phrase's length is a multiple of
// four, so it always begins on a bar and a rising block reads the same
// wherever it lands; and a switch and everything waiting on it live in the
// same phrase.
//
// Parts: a cell's bits; reading the library; the ring the course is laid
// into; choosing what comes next.

// ---- a cell's bits ---------------------------------------------------------

const C_MASK = 0x000f;          // surfaces, a bit a level
const C_DIF = 0x0080;           // a diamond, level in 8-9
const C_CRF = 0x0400;           // a creature, level in 11-12
const C_BLF = 0x2000;           // a rising block, pattern in 14-17
const C_SWF = 0x040000;         // a switch, level in 19-20
const C_SWM = 0x600000;         // what its switch does here, in 21-22
const C_SPK = 0x800000;         // the creature here has a spike on its head
const C_SPR = 0x1000000;        // a spring, on the ground only
const C_FLY = 0x2000000;        // a flyer meets the player here, level 26-27

const DI_SH = 8, CR_SH = 11, BL_SH = 14, SW_SH = 19, SWM_SH = 21, FL_SH = 26;

// What a switch does to a cell that waits on it
const SW_NONE = 0, SW_PLAT = 1, SW_GROUND = 2, SW_SWAP = 3;

// What a phrase is about, so the music can ask for one of a kind
const T_REST = 0x01, T_GAP = 0x02, T_CREATURE = 0x04, T_BLOCK = 0x08;
const T_SWITCH = 0x10, T_DIAMOND = 0x20, T_BAIT = 0x40, T_TEACH = 0x80;
const T_SPRING = 0x100, T_SPIKED = 0x200;

const PAT_MAX = 16;

// ---- reading the library ---------------------------------------------------

// Each phrase as the assembler reads it: entry, exit, difficulty, tags,
// length, and its cells packed
var pentry, pexit, pdiff, ptags, plen, pcells;
var penters;                    // whether each can be entered at each level
var count = 0;

var TAG_NAMES = ["REST", "GAP", "CREATURE", "BLOCK", "SWITCH", "DIAMOND",
                 "BAIT", "TEACH", "SPRING", "SPIKED"];

// "REST|TEACH" -> its bits
function tagsOf(s) {
    var bits = 0, at = 0;
    while (at <= s.length) {
        var end = s.indexOf("|", at);
        if (end < 0) end = s.length;
        var name = s.substring(at, end);
        var k = TAG_NAMES.indexOf(name);
        if (k < 0) {
            printh("phrases: no tag " + name);
        }
        bits |= 1 << k;
        at = end + 1;
    }
    return bits;
}

// The number in "DI(2)", from where its "(" is
function arg(s, open) {
    return parseInt(s.substring(open + 1));
}

// "G|P2|DI(2)" -> the cell, as spike_gen.c's macros pack it
function cellOf(s) {
    var bits = 0, at = 0;
    while (at < s.length) {
        var end = s.indexOf("|", at);
        if (end < 0) end = s.length;
        var w = s.substring(at, end), open = w.indexOf("(");
        var name = open < 0 ? w : w.substring(0, open);
        if (name == "__") { }
        else if (name == "G") bits |= 1;
        else if (name == "P1") bits |= 2;
        else if (name == "P2") bits |= 4;
        else if (name == "P3") bits |= 8;
        else if (name == "DI") bits |= C_DIF | (arg(w, open) << DI_SH);
        else if (name == "CR") bits |= C_CRF | (arg(w, open) << CR_SH);
        else if (name == "CR2") bits |= C_CRF | C_SPK | (arg(w, open) << CR_SH);
        else if (name == "BL") bits |= C_BLF | (arg(w, open) << BL_SH);
        else if (name == "SW") bits |= C_SWF | (arg(w, open) << SW_SH);
        else if (name == "SWP") bits |= SW_PLAT << SWM_SH;
        else if (name == "SWG") bits |= SW_GROUND << SWM_SH;
        else if (name == "SWX") bits |= SW_SWAP << SWM_SH;
        else if (name == "SPR") bits |= C_SPR;
        else if (name == "FLY") bits |= C_FLY | (arg(w, open) << FL_SH);
        else {
            printh("phrases: no cell part " + w);
        }
        at = end + 1;
    }
    return bits;
}

// The library, from its data file; phrase 0 must be bare ground, which the
// assembler falls back on
function load(list) {
    count = list.length;
    pentry = new Int32Array(count);
    pexit = new Int32Array(count);
    pdiff = new Int32Array(count);
    ptags = new Int32Array(count);
    plen = new Int32Array(count);
    pcells = new Int32Array(count * PAT_MAX);
    candidates = new Int32Array(count);
    penters = new Uint8Array(count * 4);
    for (var i = 0; i < count; i++) {
        var p = list[i];
        pentry[i] = p.entry;
        pexit[i] = p.exit;
        pdiff[i] = p.difficulty;
        ptags[i] = tagsOf(p.tags);
        plen[i] = p.cells.length;
        for (var k = 0; k < p.cells.length; k++)
            pcells[i * PAT_MAX + k] = cellOf(p.cells[k]);
        for (var l = 0; l < 4; l++)
            penters[i * 4 + l] = enters(i, l) ? 1 : 0;
    }
}

// Whether a phrase can be walked into at that level: it says so, or it has
// a surface there at both ends. Worked out once a phrase, when the library
// is read: the choosing asks it of every phrase, several times over
function patAt(i, level) {
    return penters[i * 4 + level] != 0;
}

function enters(i, level) {
    var bit = 1 << level;
    if (pentry[i] == level)
        return true;
    return (pcells[i * PAT_MAX] & bit) != 0 &&
           (pcells[i * PAT_MAX + plen[i] - 1] & bit) != 0;
}

// ---- the ring --------------------------------------------------------------

// Cells kept: the field draws a few behind the player and nine ahead
const RING = 64;

var ring = new Int32Array(RING);
var ringStart = new Int32Array(RING);   // first cell of its phrase
var filled = 0;                         // cells laid so far
var base = 0;                           // first cell still in the ring

// The run's ramp: a step every TIER_CELLS beats, the last held back
const TIER_CELLS = 32, TIER_TOP = 6, TIER_LAST = 256;

// The grid's ends, where the tempo moves the ceiling
const TEMPO_FAST = 500, TEMPO_SLOW = 720;

// A rest owed every so often, on the grid
const REST_CELLS = 48;

// Where the music's conditions bite, out of a hundred
const MOOD_HIGH = 65, MOOD_LOW = 30, MOOD_BUSY = 55, MOOD_CALM = 25;
const MOOD_RISE = 15, MOOD_DROP = -30;

// How far below the tier a phrase may still be laid
const TIER_SPREAD = 2;

var gridMs = 0;                 // the grid's period: the seed, and the band
var runBeats = 0;               // how far the run had come at runCell...
var runCell = 0;                // ...which is that cell, rotation out
var lastPick = -1;              // the phrase before, which is not offered
var lastBand = 0;               // the highest band that has opened
var lastStart = 0;              // where the phrase before began
var flat = false;               // bare ground only, while set
var lastExit = 0;               // the level the phrase before left on
var barRot = 0;                 // the residue of four the downbeat is on

var moodEnergy = 50, moodFlux = 50, moodTrend = 0;

var candidates;

// ---- choosing what comes next ----------------------------------------------

// spike_gen.c's hash, 32 bits unsigned
function hash(n) {
    n ^= n >>> 16;
    n = n * 0x7feb352d;
    n ^= n >>> 15;
    n = n * -2073254261;            // 0x846ca68b
    n ^= n >>> 16;
    return n;
}

// x % n with x read as unsigned
function umod(x, n) {
    if (x >= 0)
        return x % n;
    return ((x >>> 1) % n * 2 + (x & 1)) % n;
}

// Where the assembler is, with the bar's rotation taken out: every choice
// is a function of this, so finding the bar moves only the boundaries
function genAt(cell) {
    return cell - barRot;
}

function runAt(at) {
    var n = runBeats + (at - runCell);
    return n < 0 ? 0 : n;
}

function rampAt(beats) {
    var t = 1 + beats / TIER_CELLS;
    if (t >= TIER_TOP && beats < TIER_LAST)
        return TIER_TOP - 1;
    return t > TIER_TOP ? TIER_TOP : t;
}

function ceilingAt(beats) {
    var t = rampAt(beats);
    if (gridMs > 0 && gridMs <= TEMPO_FAST)
        t--;
    else if (gridMs >= TEMPO_SLOW)
        t++;
    return t < 1 ? 1 : t > TIER_TOP ? TIER_TOP : t;
}

// Where the bar just heard puts the course under the ceiling: a neutral
// bar one below it
function tierAt(beats) {
    var top = ceilingAt(beats), tier = top - 1;
    if (moodEnergy >= MOOD_HIGH) tier++;
    if (moodFlux >= MOOD_BUSY) tier++;
    if (moodEnergy <= MOOD_LOW) tier--;
    if (moodFlux <= MOOD_CALM) tier--;
    if (tier > top)
        tier = top;
    if (moodTrend >= MOOD_RISE && tier < TIER_TOP)
        tier++;
    return tier < 1 ? 1 : tier;
}

// The tags the music prefers, or 0: a preference among what the tier allows
function favoured() {
    var loud = moodEnergy >= MOOD_HIGH, quiet = moodEnergy <= MOOD_LOW;
    var busy = moodFlux >= MOOD_BUSY, still = moodFlux <= MOOD_CALM;
    if (loud && busy) return T_CREATURE;
    if (loud && still) return T_GAP;
    if (quiet && still) return T_REST | T_DIAMOND;
    return 0;
}

// The phrase to lay next, entered at that level; -1 for bare ground
function pick(entry) {
    var n = 0, i;
    var at = genAt(filled);
    var came = runAt(at);
    var tier = tierAt(came);
    var band = ceilingAt(came);
    var owed = at / REST_CELLS != genAt(lastStart) / REST_CELLS;

    if (moodTrend <= MOOD_DROP)
        owed = true;
    if (flat)
        return -1;

    if (owed) {
        for (i = 0; i < count; i++)
            if ((ptags[i] & T_REST) && penters[i * 4 + entry])
                candidates[n++] = i;
    }

    // A band opens on the phrase that teaches what it unlocked, once
    if (n == 0 && band > lastBand) {
        for (i = 0; i < count && band > 1; i++)
            if ((ptags[i] & T_TEACH) && penters[i * 4 + entry] && pdiff[i] == band)
                candidates[n++] = i;
        lastBand = band;
    }

    // The band, then the band without its floor, then the level alone --
    // the last only above the ground
    for (var pass = 0; pass < 3 && n == 0; pass++) {
        if (pass == 2 && entry == 0)
            break;
        for (i = 0; i < count; i++) {
            if (!penters[i * 4 + entry])
                continue;
            if (pass < 2 && pdiff[i] > tier)
                continue;
            if (pass == 0 && pdiff[i] < tier - TIER_SPREAD)
                continue;
            candidates[n++] = i;
        }
    }

    if (n > 1) {
        var want = favoured();
        if (want) {
            var keep = 0;
            for (i = 0; i < n; i++)
                if (ptags[candidates[i]] & want)
                    candidates[keep++] = candidates[i];
            if (keep)
                n = keep;
        }
    }

    // Not the one just laid
    if (n > 1) {
        var kept = 0;
        for (i = 0; i < n; i++)
            if (candidates[i] != lastPick)
                candidates[kept++] = candidates[i];
        if (kept)
            n = kept;
    }

    if (n == 0)
        return 0;

    i = candidates[umod(hash(at + gridMs * -1640531535), n)];   // 2654435761
    lastPick = i;
    lastStart = filled;
    return i;
}

function extend() {
    var at = lastExit;
    var p = pick(at), len, k;
    if (p < 0) {
        // bare ground: four cells, handed back at the level it came in on
        lastExit = 0;
        len = 4;
    } else {
        lastExit = (pentry[p] != at && patAt(p, at)) ? at : pexit[p];
        len = plen[p];
    }
    for (k = 0; k < len; k++) {
        var c = filled + k;
        ring[c % RING] = p < 0 ? 1 : pcells[p * PAT_MAX + k];
        ringStart[c % RING] = filled;
    }
    filled += len;
    if (filled - base > RING)
        base = filled - RING;
}

// Start a course at that cell; everything before it is forgotten
function reset(cell) {
    flat = false;
    lastPick = -1;
    lastExit = 0;
    filled = cell - (cell % 4) + barRot - 4;
    while (filled < 0)
        filled += 4;
    base = filled;
    lastStart = filled;
    runBeats = 0;
    runCell = genAt(filled);
}

// How far the run has come, and the cell that was true at
function setRun(beats, cell) {
    if (beats == 0)
        lastBand = 0;
    runBeats = beats;
    runCell = genAt(cell);
}

function setMood(energy, flux, trend) {
    moodEnergy = energy;
    moodFlux = flux;
    moodTrend = trend;
}

function setBar(rot) { barRot = rot & 3; }
function bar() { return barRot; }
function setTempo(ms) { gridMs = ms; }
function setFlat(on) { flat = on; }
function nextCell() { return filled; }

// Lay the next phrase now where the course runs out within ahead cells of
// cell c: called on a tick with nothing else to do, so laying one is not
// left to the tick that crosses a beat
function layAhead(c, ahead) {
    if (filled < c + ahead)
        extend();
}

// The cell at that index, laying more course as it is reached
function cell(c) {
    if (c < base)
        return 1;
    while (c >= filled)
        extend();
    return ring[c % RING];
}

function patternStart(c) {
    if (c < base)
        return c;
    while (c >= filled)
        extend();
    return ringStart[c % RING];
}

// Diamonds in the phrase a cell belongs to
function patternDiamonds(c) {
    var start = patternStart(c), n = 0;
    for (var i = 0; i < PAT_MAX; i++) {
        if (i && patternStart(start + i) != start)
            break;
        if (cell(start + i) & C_DIF)
            n++;
    }
    return n;
}
