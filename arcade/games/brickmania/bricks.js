// The wall: a grid of COLS x ROWS bricks, how they are hit, crack and
// break, and how they are drawn.
//
// Each cell holds a brick's kind (0 for none), its health and its colours
// (for plain bricks). A row can slide from side to side (levels.js says
// which), and on one level the whole wall creeps down.

// The kinds of brick
const K_NONE = 0, K_PLAIN = 1, K_STRONG = 2, K_ARMOUR = 3;
const K_METAL = 4, K_BOMB = 5, K_POWER = 6;

// By kind: hits it takes (0: never breaks), and points when it breaks
var HEALTH = [0, 1, 2, 3, 0, 1, 1];
var POINTS = [0, 10, 20, 40, 0, 15, 25];

// Any breakable brick may drop a power-up, one time in this many;
// a power brick always does
const DROP_ONE_IN = 20;

// An explosion sets off its neighbours this many ticks later, so a chain
// of explosive bricks goes off one after another rather than all at once
const BLAST_DELAY = 2;

// At most this many blasts go off in one tick, the rest the ticks after:
// a big chain reads as a sequence, and no one tick has all its debris
const BLASTS_A_TICK = 3;

var kind = new Uint8Array(ROWS * COLS);
var health = new Uint8Array(ROWS * COLS);
var colours = new Uint8Array(ROWS * COLS);     // 0-5, for plain bricks
var left = 0;                                  // breakable bricks remaining

// Bricks flashing white after a hit: which, and for how many more ticks.
// A short list rather than a flag on every brick, so a tick need not look
// at all of them.
const FLASHES = 16;
var flash_cell = new Int32Array(FLASHES), flash_ticks = new Int32Array(FLASHES);
var flashes = 0;

// Sliding rows: each row's offset in pixels, its speed, and how far it
// may go either way (0 for a row that stays put); and a list of the rows
// that slide
var row_x = [], row_speed = [], row_range = [];
var sliding = [];

// The creeping wall: pixels it has come down, and ticks to the next pixel
var drop = 0, descend_every = 0, descend_wait = 0;
const DROP_MOST = 30;           // it stops well above the paddle

// Explosions waiting to go off: the brick, and ticks until it does
const BLASTS = 64;
var blast_cell = new Int32Array(BLASTS), blast_wait = new Int32Array(BLASTS);
var blasts = 0;

// The bricks that stay put are drawn once into this picture, which is put
// on the screen each frame with one blit; when one of them changes, only
// its cell is drawn again. A sliding row is drawn brick by brick, as it
// moves. picture_rows is how many rows of the picture hold bricks, so the
// empty ones below are not copied every frame.
var picture = null;
var picture_stale = true;
var picture_rows = 0;

function setup() {
    picture = img(COLS * BRICK_W, ROWS * BRICK_H);
    for (var r = 0; r < ROWS; r++) {
        row_x.push(0.0);
        row_speed.push(0.0);
        row_range.push(0);
    }
}

// ---- building a wall (levels.js) --------------------------------------------

function clear() {
    fill(kind, K_NONE);
    flashes = 0;
    sliding = [];
    for (var r = 0; r < ROWS; r++) {
        row_x[r] = 0.0;
        row_speed[r] = 0.0;
        row_range[r] = 0;
    }
    left = 0;
    blasts = 0;
    drop = 0;
    descend_every = 0;
    picture_stale = true;
}

function place(r, c, k, plain_colours) {
    var i = r * COLS + c;
    kind[i] = k;
    health[i] = HEALTH[k];
    colours[i] = plain_colours;
    if (HEALTH[k] > 0) left++;
}

function set_motion(r, range, speed) {
    row_range[r] = range;
    row_speed[r] = speed;
    sliding.push(r);
}

function set_descent(ticks) {
    descend_every = ticks;
    descend_wait = ticks;
}

// ---- where the bricks are ---------------------------------------------------

// The top of the wall, as it creeps down
function top() {
    return WALL_Y + drop;
}

// The row at screen y, or -1 off the wall
function row_at(y) {
    var r = flr((y - top()) / BRICK_H);
    return r >= 0 && r < ROWS ? r : -1;
}

// The column at screen x in row r, allowing for the row's slide; -1 off it
function col_at(x, r) {
    var c = flr((x - WALL_X - row_x[r]) / BRICK_W);
    return c >= 0 && c < COLS ? c : -1;
}

// The brick under screen point x, y: its index, or -1 for none
function at(x, y) {
    var r = row_at(y);
    if (r < 0) return -1;
    var c = col_at(x, r);
    if (c < 0) return -1;
    var i = r * COLS + c;
    return kind[i] != K_NONE ? i : -1;
}

// A brick's top left corner on the screen
function left_of(i) {
    var r = flr(i / COLS);
    return WALL_X + (i % COLS) * BRICK_W + row_x[r];
}

function top_of(i) {
    return top() + flr(i / COLS) * BRICK_H;
}

function is_metal(i) {
    return kind[i] == K_METAL;
}

// ---- the sheet ----------------------------------------------------------------

// Bricks sit on the sheet in 16 x 8 slots, eight to a row:
//   0-5 the plain colourss, 6-7 strong (whole, cracked), 8-10 armoured
//   (whole, one crack, two), 11 metal, 12 explosive, 13 power, 14 the
//   white of a flash

// The slot showing brick i as it is now
function slot(i) {
    var k = kind[i];
    if (k == K_PLAIN) return colours[i];
    if (k == K_STRONG) return health[i] == 2 ? 6 : 7;
    if (k == K_ARMOUR) return 8 + (3 - health[i]);
    if (k == K_METAL) return 11;
    if (k == K_BOMB) return 12;
    return 13;                  // K_POWER
}

// The slot of brick i undamaged, whose colourss its sparks and debris take
function whole_slot(i) {
    var k = kind[i];
    if (k == K_PLAIN) return colours[i];
    if (k == K_STRONG) return 6;
    if (k == K_ARMOUR) return 8;
    return slot(i);
}

function slot_x(s) { return (s % 8) * 16; }
function slot_y(s) { return flr(s / 8) * 8; }

// A brick's highlight and body colourss, read from its picture on the
// sheet, so that recoloursing a brick there recolourss its sparks and
// debris too. Debris takes the three colourss from the highlight on, so a
// brick's highlight, body and shadow sit in a row in the palette.
function highlight(i) { var s = whole_slot(i); return sget(slot_x(s) + 3, slot_y(s)); }
function body(i)      { var s = whole_slot(i); return sget(slot_x(s) + 1, slot_y(s) + 1); }

// ---- hitting bricks -----------------------------------------------------------

// Brick i struck for `damage` at x, y by something moving dx, dy. Metal
// shrugs it off; anything else flashes, chips, and cracks or breaks.
// Returns true when the brick broke.
function hit(i, damage, x, y, dx, dy) {
    if (kind[i] == K_METAL) {
        fx.impact(fx.IMP_METAL, x, y, C_WHITE, -dx, -dy);
        return false;
    }
    start_flash(i);
    if (damage >= health[i]) {
        smash(i, x, y, dx, dy);
        return true;
    }
    health[i] -= damage;
    repaint(i);                 // a crack to draw
    fx.impact(fx.IMP_CRACK, x, y, body(i), -dx, -dy);
    fx.chips(2 + rnd(4), x, y, body(i), -dx, -dy);
    return false;
}

// Brick i breaks: it fractures into debris, scores, may drop a power-up,
// and, if explosive, sets off its neighbours
function smash(i, x, y, dx, dy) {
    var k = kind[i];
    var bx = left_of(i), by = top_of(i);
    var cx = bx + BRICK_W / 2, cy = by + BRICK_H / 2;
    var c_body = body(i), c_ramp = highlight(i);

    kind[i] = K_NONE;
    left--;
    repaint(i);
    fx.impact(k == K_ARMOUR ? fx.IMP_ARMOUR : fx.IMP_BREAK, x, y, c_body, dx, dy);
    fx.fracture(bx, by, c_ramp, dx, k == K_ARMOUR);
    award(POINTS[k], cx - 4, cy - 4);
    balls.speed_up();

    if (k == K_POWER || rnd(DROP_ONE_IN) == 0) powers.drop(cx - 5, cy);
    if (k == K_BOMB) explode(i, cx, cy);
    if (left == 0) level_cleared();
}

// An explosive brick goes off at cx, cy: a flash, a ring and a blast, and
// each of its eight neighbours is hit BLAST_DELAY ticks later
function explode(i, cx, cy) {
    fx.impact(fx.IMP_BOOM, cx, cy, C_FIRE, 0, 0);
    fx.ring(cx, cy, C_FIRE, 14);
    fx.flash(cx, cy);
    var r = flr(i / COLS), c = i % COLS;
    for (var dr = -1; dr <= 1; dr++)
        for (var dc = -1; dc <= 1; dc++) {
            var nr = r + dr, nc = c + dc;
            if ((dr != 0 || dc != 0) && nr >= 0 && nr < ROWS &&
                nc >= 0 && nc < COLS && kind[nr * COLS + nc] != K_NONE)
                schedule(nr * COLS + nc);
        }
}

function schedule(i) {
    if (blasts == BLASTS) return;           // a full queue: let it go
    blast_cell[blasts] = i;
    blast_wait[blasts] = BLAST_DELAY;
    blasts++;
}

// ---- every tick ---------------------------------------------------------------

function update() {
    update_flashes();

    // sliding rows turn round at the ends of their range
    for (var k = 0; k < sliding.length; k++) {
        var r = sliding[k];
        row_x[r] += row_speed[r];
        if (abs(row_x[r]) >= row_range[r]) {
            row_x[r] = mid(-row_range[r], row_x[r], row_range[r]);
            row_speed[r] = -row_speed[r];
        }
    }

    // the creeping wall
    if (descend_every > 0 && drop < DROP_MOST) {
        descend_wait--;
        if (descend_wait <= 0) {
            drop++;
            descend_wait = descend_every;
        }
    }

    update_blasts();
}

// Explosions whose time has come go off. They are taken out of the queue
// first, since each hit may queue more explosions of its own.
var ready = new Int32Array(BLASTS);

function update_blasts() {
    var kept = 0, n = 0;
    for (var q = 0; q < blasts; q++) {
        if (blast_wait[q] > 1 || n == BLASTS_A_TICK) {
            blast_cell[kept] = blast_cell[q];   // not yet: stays queued
            blast_wait[kept] = max(blast_wait[q] - 1, 1);
            kept++;
        } else
            ready[n++] = blast_cell[q];
    }
    blasts = kept;
    for (var k = 0; k < n; k++) {
        var i = ready[k];
        if (kind[i] != K_NONE)
            hit(i, 1, left_of(i) + BRICK_W / 2, top_of(i) + BRICK_H / 2, 0, 0);
    }
}

// ---- flashes ------------------------------------------------------------------

// Brick i shows white for two ticks (one may not be drawn, if the player
// runs late); with the list full, it simply does not flash
function start_flash(i) {
    if (flashes == FLASHES) return;
    flash_cell[flashes] = i;
    flash_ticks[flashes] = 2;
    flashes++;
}

// Each flash a tick shorter; finished ones taken out of the list
function update_flashes() {
    var kept = 0;
    for (var f = 0; f < flashes; f++)
        if (flash_ticks[f] > 1) {
            flash_cell[kept] = flash_cell[f];
            flash_ticks[kept] = flash_ticks[f] - 1;
            kept++;
        }
    flashes = kept;
}

// ---- drawing --------------------------------------------------------------------

function draw() {
    if (picture_stale) redraw_picture();
    if (picture_rows > 0)
        blitpart(picture, 0, 0, COLS * BRICK_W, picture_rows * BRICK_H,
                 WALL_X, top());

    // the sliding rows
    for (var k = 0; k < sliding.length; k++) {
        var r = sliding[k];
        for (var c = 0; c < COLS; c++) {
            var i = r * COLS + c;
            if (kind[i] != K_NONE) draw_brick(slot(i), left_of(i), top_of(i));
        }
    }
    // flashes, over the brick (one just broken flashes where it was)
    for (var f = 0; f < flashes; f++) {
        var i = flash_cell[f];
        draw_brick(14, left_of(i), top_of(i));
    }
}

// The whole picture again, at the start of a level
function redraw_picture() {
    target(picture);
    cls(0);
    picture_rows = 0;
    for (var r = 0; r < ROWS; r++) {
        if (row_range[r] > 0) continue;
        for (var c = 0; c < COLS; c++) {
            var i = r * COLS + c;
            if (kind[i] != K_NONE) {
                draw_brick(slot(i), c * BRICK_W, r * BRICK_H);
                picture_rows = r + 1;
            }
        }
    }
    target(SCREEN);
    picture_stale = false;
}

// Brick i's cell of the picture drawn again, as it cracks or breaks. A
// sliding row's bricks are not in the picture, so are left alone.
function repaint(i) {
    var r = flr(i / COLS), c = i % COLS;
    if (row_range[r] > 0 || picture_stale) return;
    var x = c * BRICK_W, y = r * BRICK_H;
    target(picture);
    rectfill(x, y, x + BRICK_W - 1, y + BRICK_H - 1, 0);   // see-through
    if (kind[i] != K_NONE) draw_brick(slot(i), x, y);
    target(SCREEN);
}

function draw_brick(s, x, y) {
    sspr(slot_x(s), slot_y(s), BRICK_W, BRICK_H, x, y);
}
