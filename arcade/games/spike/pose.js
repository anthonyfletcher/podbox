// The character: the easing the world scrolls on, the pose tables the
// triangle is drawn from (data/poses.json), and the curves that carry it
// between levels.
//
// Every animation resolves exactly on a beat boundary: each table is read
// by the sub-beat phase, 0..PHASE-1, so the frame rate can vary without the
// character arriving late. A pose is seven numbers -- apex_dx, half_width,
// height, rotation (degrees about the centroid), base_tilt (degrees; its
// sign is the foot), y_offset and x_offset -- in the field's own pixels,
// twice the screen's.
//
// Parts: the easing; the tables; reading a table; the curves between
// levels; the triangle.

const PHASE = 256;
const ROWS = 16;
const IDLE_MOVES = 12;

// ---- the easing ------------------------------------------------------------

// 1-(1-t)^3 at sixteenths: most of the cell is covered by three fifths of
// the beat, and the rest is the settle
var easeTable = [0, 45, 85, 119, 148, 173, 194, 210, 224, 235, 243, 248,
                 252, 254, 256, 256, 256];

function ease(phase) {
    var i = phase >> 4, frac = phase & 15;
    return easeTable[i] + (((easeTable[i + 1] - easeTable[i]) * frac) >> 4);
}

// ---- the tables --------------------------------------------------------------

var hopT, idleT, jumpT, landT, ouchT, fallT, sproutT;

function table(rows) {
    var t = new Int32Array(rows.length * 7);
    for (var i = 0; i < rows.length; i++)
        for (var k = 0; k < 7; k++)
            t[i * 7 + k] = rows[i][k];
    return t;
}

function load(d) {
    hopT = table(d.hop);
    idleT = table(d.idle);
    jumpT = table(d.jump);
    landT = table(d.land);
    ouchT = table(d.ouch);
    fallT = table(d.fall);
    sproutT = table(d.sprout);
}

function newPose() {
    return { adx: 0, hw: 0, h: 0, rot: 0, tilt: 0, yo: 0, xo: 0 };
}

function rowOf(p, t, i) {
    var k = i * 7;
    p.adx = t[k]; p.hw = t[k + 1]; p.h = t[k + 2]; p.rot = t[k + 3];
    p.tilt = t[k + 4]; p.yo = t[k + 5]; p.xo = t[k + 6];
}

// ---- reading a table ---------------------------------------------------------

function row(phase, rows) {
    var i = (phase * rows) >> 8;
    return i < 0 ? 0 : i > rows - 1 ? rows - 1 : i;
}

// The weak beat lands on the other corner, and lighter
function foot(tilt, strong) {
    return strong ? tilt : -(tilt * 2) / 3;
}

function idle(p, phase, move) {
    rowOf(p, idleT, (move % IDLE_MOVES) * ROWS + row(phase, ROWS));
}

// Moves 2 and 6 end held, and name the one that follows
function held(move) {
    return move == 2 || move == 6;
}

function idleNext(prev) {
    if (held(prev))
        return prev + 1;
    var n;
    do
        n = flr(rnd(IDLE_MOVES));
    while (n == prev || held(n - 1));
    return n;
}

function hop(p, phase, strong) {
    rowOf(p, hopT, row(phase, ROWS));
    p.tilt = foot(p.tilt, strong);
}

// ride: how far above the surface the body arrives -- the height of what
// it came down on and pushes flat; 0 for an ordinary landing
function landed(p, phase, strong, ride) {
    var r = row(phase, ROWS);
    rowOf(p, landT, r);
    p.tilt = foot(p.tilt, strong);
    if (ride <= 0)
        return;
    if (r < 4) {
        p.yo = -(ride * (4 - r)) / 4;
        p.hw = p.hw + r / 2;
        p.h = p.h - r;
    } else if (r < 7) {
        var pop = ride / 2;
        p.yo = -pop + (pop * (r - 4)) / 3;
    }
}

function sprout(p, phase, landStrong) {
    rowOf(p, sproutT, row(phase, ROWS));
    p.tilt = foot(p.tilt, landStrong);
}

function jump(p, phase, landStrong) {
    rowOf(p, jumpT, row(phase, ROWS * 2));
    p.tilt = foot(p.tilt, landStrong);
}

function ouch(p, phase) {
    rowOf(p, ouchT, row(phase, ROWS));
}

// A jump that came up short was already falling: it skips the held beat
function fall(p, phase, fromAir, strong) {
    var skip = fromAir ? 4 : 0;
    rowOf(p, fallT, row(phase, ROWS - skip) + skip);
    p.tilt = foot(p.tilt, strong);
}

// ---- the curves between levels -------------------------------------------------

// The level during a step, 8.8: up follows the scroll, down accelerates
function walkLevel(from, to, phase) {
    if (to < 0)
        to = from;
    var f = to >= from ? ease(phase) : (phase * phase) >> 8;
    return (from << 8) + (to - from) * f;
}

// ...and during an airborne move, peaking on apex whatever the far side
function arcLevel(from, to, phase, apex) {
    if (to < 0)
        to = 0;
    var lin = (from << 8) + (to - from) * phase;
    var bell = (4 * phase * (PHASE - phase)) >> 8;
    var amp = ((apex - from) << 8) - ((to - from) << 7);
    return lin + ((amp * bell) >> 8);
}

// ---- the triangle ------------------------------------------------------------

// The three corners, base centred on bx, by, into px and py
function points(p, bx, by, px, py) {
    var pivot = p.tilt >= 0 ? p.hw : -p.hw;
    px[0] = -p.hw; py[0] = 0;
    px[1] = p.hw;  py[1] = 0;
    px[2] = p.adx; py[2] = -p.h;
    // tilt about the foot's corner, then spin about the centroid; sin is
    // the screen's, so a positive angle turns clockwise as the C did
    var ts = -sin(p.tilt / 360.0), tc = cos(p.tilt / 360.0);
    var rs = -sin(p.rot / 360.0), rc = cos(p.rot / 360.0);
    var cx = p.adx / 3, cy = -p.h / 3;
    for (var i = 0; i < 3; i++) {
        var x = px[i], y = py[i], dx, dy;
        if (p.tilt != 0) {
            dx = x - pivot; dy = y;
            x = pivot + dx * tc - dy * ts;
            y = dx * ts + dy * tc;
        }
        if (p.rot != 0) {
            dx = x - cx; dy = y - cy;
            x = cx + dx * rc - dy * rs;
            y = cy + dx * rs + dy * rc;
        }
        px[i] = x + bx + p.xo;
        py[i] = y + by + p.yo;
    }
}
