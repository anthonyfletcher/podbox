// The phrase checker: whether each phrase in the library can be crossed,
// by the game's own rules (world.js) on a course of the phrase alone. The
// phrase editor runs it, with a main.js of its own, through the IDE: the
// game never calls it, so it is left out of the game's build.
//
// Each phrase is checked at every level it can be entered at: laid between
// a run-up at that level and a run-off at the level it must leave on, and
// played by trying presses on the cell before it and on each of its own --
// the fewest first, so the line reported is the plainest one. A phrase no
// line crosses is a fault, and so is one laid out against the rules the
// rest of the game takes for granted. One phrase a tick, printed as
//
//   phrase <i> ok <level> <the beats pressed, from the phrase's first>
//   phrase <i> fault <level> <why>
//   phrase <i> layout <why>
//
// and "done" after the last.

const RUN_UP = 4, RUN_OFF = 4;
const START = RUN_UP;                   // the phrase's first cell
const MOST = 60000;                     // lines tried, at most, at one level

var list, at = 0, goal = 0, exitLevel = 0, len = 0, tried = 0;
var pressed = new Int32Array(24), presses = 0;
var states = [], savedCells = [], savedBits = [];

function begin(phrases) {
    list = phrases;
    gen.load(phrases);
    for (var d = 0; d < 40; d++) {
        states.push(world.newState());
        savedCells.push(new Int32Array(world.MARKS));
        savedBits.push(new Int32Array(world.MARKS));
    }
}

// One phrase a tick
function step() {
    if (at >= list.length) {
        if (at == list.length)
            printh("done");
        at = list.length + 1;
        return;
    }
    layout(at);
    for (var level = 0; level < world.LEVELS; level++)
        if (gen.patAt(at, level))
            cross(at, level);
    at++;
}

// The rules a phrase's cells are laid out by, which the game takes for
// granted rather than checking as it goes
function layout(i) {
    var n = gen.plen[i], hasSwitch = false, waits = false;
    if (n % 4 != 0 || n > gen.PAT_MAX)
        printh("phrase " + i + " layout its length, " + n + ", is not a multiple of four up to sixteen");
    for (var k = 0; k < n; k++) {
        var e = gen.pcells[i * gen.PAT_MAX + k];
        if (e & gen.C_SWF) hasSwitch = true;
        if (e & gen.C_SWM) waits = true;
        if (e & gen.C_FLY) {
            var fl = (e >> gen.FL_SH) & 3;
            // it flies in over the three cells after its own
            if (k > n - 4)
                printh("phrase " + i + " layout the flyer at cell " + k + " has fewer than three cells after it to fly in over");
            for (var j = k + 1; j <= k + 3 && j < n; j++)
                if (fl + 1 < world.LEVELS && (gen.pcells[i * gen.PAT_MAX + j] & (1 << (fl + 1))))
                    printh("phrase " + i + " layout the flyer at cell " + k + " flies in under a platform at cell " + j);
        }
    }
    if (waits && !hasSwitch)
        printh("phrase " + i + " layout cells wait on a switch, and it has none");
}

// The phrase alone on the course: a run-up at level, the phrase, a run-off
// at the level it should leave on
function lay(i, level) {
    var n = gen.plen[i];
    len = n;
    exitLevel = (gen.pentry[i] != level && gen.patAt(i, level)) ? level : gen.pexit[i];
    gen.setBar(0);
    for (var c = 0; c < gen.RING; c++) {
        var v, s;
        if (c < START) { v = 1 << level; s = 0; }
        else if (c < START + n) { v = gen.pcells[i * gen.PAT_MAX + c - START]; s = START; }
        else { v = 1 << exitLevel; s = START + n; }
        gen.ring[c] = v;
        gen.ringStart[c] = s;
    }
    gen.base = 0;
    gen.filled = gen.RING;
    goal = START + n + RUN_OFF - 1;
}

function cross(i, level) {
    lay(i, level);
    tried = 0;
    // the fewest presses first: no line with fewer was found
    for (var most = 0; most <= len + 1; most++) {
        world.forget();
        var st = states[0];
        world.startAt(st, START - 2, level);
        presses = 0;
        if (search(0, most)) {
            var s = "";
            for (var k = 0; k < presses; k++)
                s = s + (k ? "," : "") + (pressed[k] - START);
            printh("phrase " + i + " ok " + level + " " + s);
            return;
        }
        if (tried >= MOST) {
            printh("phrase " + i + " fault " + level + " gave up after " + tried + " lines");
            return;
        }
    }
    printh("phrase " + i + " fault " + level + " no line crosses it, entered at level " + level +
           " to leave at level " + exitLevel);
}

// Whether the line from states[d] reaches the end standing at the exit
// level, pressing at most most more times
function search(d, most) {
    var st = states[d];
    if (st.beat >= goal)
        return st.level == exitLevel;
    if (++tried >= MOST)
        return false;
    var canPress = most > 0 && st.motion == world.WALK &&
                   st.beat >= START - 1 && st.beat < START + len;
    for (var press = 0; press < (canPress ? 2 : 1); press++) {
        var nx = states[d + 1];
        world.copyState(nx, st);
        copy(savedCells[d], 0, world.markCell, 0, world.MARKS);
        copy(savedBits[d], 0, world.markBits, 0, world.MARKS);
        var sp = world.switchPattern;
        if (press) {
            world.jump(nx);
            pressed[presses++] = st.beat;
        }
        if (world.advance(nx) == world.OUT_OK && search(d + 1, most - press))
            return true;
        if (press)
            presses--;
        copy(world.markCell, 0, savedCells[d], 0, world.MARKS);
        copy(world.markBits, 0, savedBits[d], 0, world.MARKS);
        world.switchPattern = sp;
    }
    return false;
}
