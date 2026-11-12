// Effects: everything that is only for show.
//
//   impact()    one call for every collision's feedback: sparks, shake,
//               hit-stop and sound, by a table of kinds below
//   sparks and chips   the engine's own particle pools, a pixel each
//   fragments   pieces of a broken brick, tumbling and falling
//   rings, floating text, a flash, and the stars behind it all
//
// Every pool is a fixed size. When one is full, a new effect is simply
// left out (floating text replaces its oldest) -- the game itself never
// waits on an effect.
//
// A chain of explosions can break a dozen bricks in one tick, and their
// effects together would make that tick late. So after CALM bricks have
// broken in a tick, the rest that tick are quieter: fewer sparks, no dust,
// no score text (see busy()), and two pieces of debris instead of up to
// eight. The spec's order: dust, then text, then fragments.

// ---- the impact table --------------------------------------------------------
// One row a kind of collision. Tune the feel of the game here.

const IMP_WALL = 0, IMP_PADDLE = 1, IMP_CRACK = 2, IMP_BREAK = 3;
const IMP_ARMOUR = 4, IMP_METAL = 5, IMP_BOOM = 6, IMP_POWERUP = 7;
const IMP_POWERED = 8;

//                 wall paddle crack break armour metal boom power powered
var SPARKS =      [2,   4,     4,    10,   15,    6,    25,  12,   5];
var SHAKE_PIXELS = [0,  0,     1,    1,    1,     1,    2,   1,    1];
var SHAKE_TICKS = [0,   0,     1,    2,    2,     1,    4,   2,    1];
var STOP_TICKS =  [0,   0,     1,    0,    1,     0,    2,   2,    0];
var SOUNDS = [SFX.wall, SFX.paddle, SFX.crack, SFX.smash, SFX.smash,
             SFX.metal, SFX.boom, SFX.power, SFX.hit];

// Feedback for a collision of `kind` at x, y, in colour c, the sparks
// flying off along dx, dy. A long combo throws more sparks and raises the
// pitch of brick sounds.
function impact(kind, x, y, c, dx, dy) {
    var n = SPARKS[kind];
    var pitch = rnd(3) - 1;             // a step either way, for variety
    if (kind >= IMP_CRACK && kind <= IMP_ARMOUR) {
        n += combo;
        pitch += combo;
    }
    if (busy()) n = flr(n / 3);
    sparks(n, x, y, dx * 0.5, dy * 0.5, 0.9, 10, c);
    if (kind == IMP_METAL) sparks(4, x, y, dx, dy, 1.5, 6, C_WHITE);
    shake(SHAKE_PIXELS[kind], SHAKE_TICKS[kind]);
    stop(STOP_TICKS[kind]);
    sfxp(SOUNDS[kind], pitch);
}

// ---- how busy this tick is ------------------------------------------------------

const CALM = 3;                 // bricks a tick broken with every effect
var broken = 0;                 // bricks broken so far this tick

function busy() {
    return broken > CALM;
}

// ---- the particle pools ---------------------------------------------------------
// Sparks drift and slow; chips fall; debris falls and tumbles. The engine
// moves and draws each pool at once. Sparks and chips are a pixel each;
// debris is a sprite (see fracture below).

var spark_pool = null, chip_pool = null, debris = null;

function setup() {
    spark_pool = particles.pool(64);
    spark_pool.gravity = 0.0;
    spark_pool.drag = 0.08;
    chip_pool = particles.pool(32);
    chip_pool.gravity = 0.08;
    chip_pool.drag = 0.01;
    debris = particles.pool(48);
    debris.gravity = 0.08;
    debris.w = 5;                   // each piece fits in its sprite's top
    debris.h = 5;                   // left 5 x 5
    debris.shade = 3;               // colours 1 2 3: the brick's ramp
    debris.floor = FIELD_B - 3;     // one bounce off the bottom...
    debris.bounce = 0.35;           // ...then too slow to bounce again
    debris.spin = 5;                // a quarter turn every 5 ticks
    for (var s = 0; s < STARS; s++) {
        star_x.push(rnd(160.0));
        star_y.push(HUD_H + rnd(110.0));
        star_speed.push(0.1 + rnd(0.1));
    }
}

function sparks(n, x, y, dx, dy, spread, life, c) {
    particles.emit(spark_pool, n, x, y, dx, dy, spread, life, c);
}

function chips(n, x, y, c, dx, dy) {
    particles.emit(chip_pool, n, x, y, dx * 0.3, -1.0, 0.8, 30, c);
}

// ---- screen shake and hit-stop ------------------------------------------------------
// Shake moves the world (not the HUD) by up to a few pixels for a few
// ticks; hit-stop freezes the game for a tick or two after a big hit.

var shake_x = 0, shake_y = 0;
var shake_pixels = 0, shake_ticks = 0;
var stop_ticks = 0, frozen = false;

function shake(pixels, ticks) {
    if (pixels >= shake_pixels || shake_ticks == 0) shake_pixels = pixels;
    shake_ticks = max(shake_ticks, ticks);
}

function stop(ticks) {
    stop_ticks = max(stop_ticks, ticks);
}

// Whether this tick is frozen by hit-stop
function stopped() {
    return frozen;
}

// ---- debris -----------------------------------------------------------------------
// Pieces of a broken brick: the six shapes on the sheet at
// y 40 of the sheet (sprites 80 to 85), drawn there in colours 1, 2 and 3.
// The debris pool draws each piece in the colour ramp it was given -- the
// broken brick's highlight, body and shadow, three in a row in the palette
// -- flipped at random, turning as it falls.

const FIRST_PIECE = 80, PIECES = 6;

// A brick at x, y breaks into pieces in its colour ramp (its highlight,
// then body and shadow), bursting away the way the ball was going (dx) and
// up, then falling. An armoured brick breaks into more.
function fracture(x, y, ramp, dx, big) {
    broken++;
    var pieces = big ? 8 : 4 + rnd(3);
    if (busy()) pieces = 2;
    for (var p = 0; p < pieces; p++)
        particles.emit(debris, 1, x + rnd(9.0), y + rnd(3.0), dx * 0.3, -1.15,
                       1.1, 50, ramp, FIRST_PIECE + rnd(PIECES));
    // and dust: smaller bits in the body and highlight colours
    if (busy()) return;
    chips(4 + rnd(4), x + 6, y + 3, ramp + 1, dx, 0);
    chips(2 + rnd(3), x + 6, y + 3, ramp, dx, 0);
}

// ---- rings ----------------------------------------------------------------------------

const RINGS = 8;
var ring_on = new Uint8Array(RINGS), ring_x = new Int32Array(RINGS);
var ring_y = new Int32Array(RINGS), ring_r = new Int32Array(RINGS);
var ring_most = new Int32Array(RINGS), ring_c = new Uint8Array(RINGS);

// A circle growing out from x, y to `most` pixels across, then gone
function ring(x, y, c, most) {
    for (var i = 0; i < RINGS; i++)
        if (!ring_on[i]) {
            ring_on[i] = 1;
            ring_x[i] = x; ring_y[i] = y; ring_r[i] = 1;
            ring_most[i] = most; ring_c[i] = c;
            return;
        }
}

// ---- floating text --------------------------------------------------------------------
// Scores and names that rise a few pixels, flash, then fade, each over
// a shadow

const TEXTS = 12;
const TEXT_LIFE = 24;
var text_on = new Uint8Array(TEXTS), text_life = new Int32Array(TEXTS);
var text_c = new Uint8Array(TEXTS);
var text_str = ["", "", "", "", "", "", "", "", "", "", "", ""];
var text_x = balls.zeros(TEXTS), text_y = balls.zeros(TEXTS);

function text(s, x, y, c) {
    var i = 0;
    for (var k = 1; k < TEXTS; k++)         // a free one, or else the oldest
        if (text_life[k] < text_life[i]) i = k;
    text_on[i] = 1;
    text_life[i] = TEXT_LIFE;
    text_x[i] = mid(FIELD_L, x, FIELD_R - textw(s));
    text_y[i] = y;
    text_c[i] = c;
    text_str[i] = s;
}

// ---- the flash ---------------------------------------------------------------------------
// One tick of a white disc where something exploded

var flash_x = 0, flash_y = 0, flash_ticks = 0;

function flash(x, y) {
    flash_x = x;
    flash_y = y;
    flash_ticks = 1;
}

// ---- the stars ---------------------------------------------------------------------------

const STARS = 24;
var star_x = [], star_y = [], star_speed = [];

// ---- the level-clear celebration -------------------------------------------------------

// Bursts of gold and white sparks across the playfield
function celebrate() {
    for (var k = 0; k < 6; k++) {
        var x = 20 + k * 24, y = 30 + rnd(40);
        sparks(8, x, y, 0, -0.4, 1.4, 50, k % 2 == 0 ? C_GOLD : C_WHITE);
        ring(x, y, k % 2 == 0 ? C_GOLD : C_WHITE, 10);
    }
}

// Every spark drawn in towards x, y, arriving in about 20 ticks
function gather(x, y) {
    var p = spark_pool;
    for (var i = 0; i < p.life.length; i++)
        if (p.life[i] > 0) {
            p.dx[i] = (x - p.x[i]) / 20.0;
            p.dy[i] = (y - p.y[i]) / 20.0;
            p.life[i] = max(p.life[i], 20);
        }
    spark_pool.drag = 0.0;
}

// ---- every tick ----------------------------------------------------------------------------

function update() {
    broken = 0;
    // hit-stop: everything waits, the shake included
    frozen = stop_ticks > 0;
    if (frozen) {
        stop_ticks--;
        return;
    }
    if (state != ST_CLEAR) spark_pool.drag = 0.08;

    if (shake_ticks > 0) {
        shake_ticks--;
        shake_x = rnd(shake_pixels * 2 + 1) - shake_pixels;
        shake_y = rnd(shake_pixels * 2 + 1) - shake_pixels;
    } else {
        shake_x = 0;
        shake_y = 0;
    }

    particles.step(spark_pool);
    particles.step(chip_pool);
    particles.step(debris);

    for (var i = 0; i < RINGS; i++)
        if (ring_on[i] && ++ring_r[i] > ring_most[i] / 2) ring_on[i] = 0;
    for (var i = 0; i < TEXTS; i++)
        if (text_on[i]) {
            text_y[i] -= 0.15;
            if (--text_life[i] <= 0) text_on[i] = 0;
        }
    for (var s = 0; s < STARS; s++) {
        star_y[s] += star_speed[s];
        if (star_y[s] >= 120) star_y[s] = HUD_H;
    }
}

// ---- drawing --------------------------------------------------------------------------------

function draw_stars() {
    for (var s = 0; s < STARS; s++)
        pset(star_x[s], star_y[s], s % 3 == 0 ? C_STAR : C_STAR_DIM);
}

// Behind the paddle and balls: the debris
function draw_behind() {
    particles.draw(debris);
    particles.draw(chip_pool);
}

// In front: sparks, rings, the flash and the floating text
function draw_front() {
    particles.draw(spark_pool);
    for (var i = 0; i < RINGS; i++)
        if (ring_on[i]) circ(ring_x[i], ring_y[i], ring_r[i], ring_c[i]);
    if (flash_ticks > 0) {
        circfill(flash_x, flash_y, 5, C_WHITE);
        flash_ticks--;
    }
    for (var i = 0; i < TEXTS; i++) {
        if (!text_on[i]) continue;
        var age = TEXT_LIFE - text_life[i];
        var c = text_c[i];
        if (age < 6 && age % 2 == 0) c = C_WHITE;   // a flash at first
        if (text_life[i] < 6) c = C_DARK;           // and a fade at the end
        // a shadow, so it reads over the bricks
        print(text_str[i], text_x[i] + 1, text_y[i] + 1, C_BACKGROUND);
        print(text_str[i], text_x[i], text_y[i], c);
    }
}
