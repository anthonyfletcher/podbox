// Power-ups: capsules that fall from broken bricks, caught with the paddle.
//
//   Multiball  each ball becomes three, up to four in all
//   Fireball   the ball goes through anything that breaks, for a while
//   Laser      Select fires two shots up from the paddle, for a while
//   Sticky     the paddle catches the ball; Select lets it go
//   Expand     a wider paddle
//   Shrink     a narrower paddle, but every brick scores double
//   Slow       the balls slow down, for a while
//   Shield     a line under the paddle that saves one ball
//
// Losing a life ends every one of them.

const P_MULTI = 0, P_FIRE = 1, P_LASER = 2, P_STICKY = 3;
const P_EXPAND = 4, P_SHRINK = 5, P_SLOW = 6, P_SHIELD = 7;

var NAMES = ["MULTIBALL", "FIREBALL", "LASER", "STICKY",
             "EXPAND", "SHRINK", "SLOW", "SHIELD"];
// each one's colour, as on its capsule
var COLOURS = [C_GOLD, C_FIRE, C_LASER, C_STICKY, 13, 9, C_SLOW, C_SHIELD];
// how likely each is to drop, out of the total: Shrink least
var CHANCE = [4, 3, 3, 3, 4, 2, 3, 3];

const SECONDS = 33;             // ticks in a second (cart.json's rate)
const TIMED = 10 * SECONDS;     // how long Fireball, Laser, Sticky and Slow last

// ---- the capsules ---------------------------------------------------------------
// 10 x 6 pixels, falling at FALL pixels a tick

const CAPSULES = 8;
const FALL = 0.6;
var cap_on = new Uint8Array(CAPSULES), cap_type = new Uint8Array(CAPSULES);
var cap_x = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
var cap_y = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0];

// ---- what is active ---------------------------------------------------------------
// The timed ones count down in ticks; 0 is off

var fire_ticks = 0, laser_ticks = 0, sticky_ticks = 0, slow_ticks = 0;
var shield = false;

// ---- the laser's shots ---------------------------------------------------------------

const SHOTS = 16;
const SHOT_SPEED = 4;
const SHOT_GAP = 8;             // ticks between pairs of shots
var shot_on = new Uint8Array(SHOTS);
var shot_x = new Int32Array(SHOTS), shot_y = new Int32Array(SHOTS);
var shot_wait = 0;

function reset() {
    fill(cap_on, 0);
    fill(shot_on, 0);
    fire_ticks = 0;
    laser_ticks = 0;
    sticky_ticks = 0;
    slow_ticks = 0;
    shield = false;
    paddle.set_width(paddle.W_NORMAL);
}

// What balls.js and paddle.js ask
function fireball() { return fire_ticks > 0; }
function laser() { return laser_ticks > 0; }
function sticky() { return sticky_ticks > 0; }
function slowed() { return slow_ticks > 0; }
function shielded() { return shield; }
function shrunk() { return paddle.target_w == paddle.W_NARROW; }

// ---- dropping and catching ------------------------------------------------------

// A capsule at x, y, of a kind picked by CHANCE; none if eight are falling
function drop(x, y) {
    for (var i = 0; i < CAPSULES; i++)
        if (!cap_on[i]) {
            cap_on[i] = 1;
            cap_type[i] = pick();
            cap_x[i] = x;
            cap_y[i] = y;
            return;
        }
}

function pick() {
    var total = 0;
    for (var k = 0; k < CHANCE.length; k++) total += CHANCE[k];
    var roll = rnd(total);
    for (var type = 0; type < CHANCE.length; type++) {
        if (roll < CHANCE[type]) return type;
        roll -= CHANCE[type];
    }
    return 0;
}

function update() {
    for (var i = 0; i < CAPSULES; i++) {
        if (!cap_on[i]) continue;
        cap_y[i] += FALL;
        // caught: its box overlapping the paddle's
        if (cap_y[i] + 6 >= PADDLE_Y && cap_y[i] <= PADDLE_Y + 4 &&
            cap_x[i] + 10 >= paddle.left() && cap_x[i] <= paddle.right()) {
            cap_on[i] = 0;
            collect(cap_type[i]);
        } else if (cap_y[i] > FIELD_B)
            cap_on[i] = 0;
    }
    if (fire_ticks > 0) fire_ticks--;
    if (laser_ticks > 0) laser_ticks--;
    if (sticky_ticks > 0) sticky_ticks--;
    if (slow_ticks > 0 && --slow_ticks == 0) speed_all();
    if (shot_wait > 0) shot_wait--;
    update_shots();
}

// A power-up caught: its effect, then a burst of its colour from the
// paddle, its name, and a moment's pause for the big ones
function collect(type) {
    var px = paddle.x, py = PADDLE_Y;
    if (type == P_MULTI) {
        balls.split();
        fx.ring(px, py, C_GOLD, 16);
    } else if (type == P_FIRE)
        fire_ticks = TIMED;
    else if (type == P_LASER)
        laser_ticks = TIMED;
    else if (type == P_STICKY)
        sticky_ticks = TIMED;
    else if (type == P_EXPAND)
        paddle.set_width(paddle.W_WIDE);
    else if (type == P_SHRINK)
        paddle.set_width(paddle.W_NARROW);
    else if (type == P_SLOW) {
        slow_ticks = TIMED;
        speed_all();
    } else if (type == P_SHIELD)
        shield = true;

    fx.impact(fx.IMP_POWERUP, px, py, COLOURS[type], 0, -1);
    fx.text(NAMES[type], px - 18, py - 10, COLOURS[type]);
    if (type == P_MULTI || type == P_FIRE || type == P_LASER) fx.stop(2);
}

// Every ball's speed set again, as Slow starts or ends
function speed_all() {
    for (var b = 0; b < balls.MOST; b++)
        if (balls.on[b] && !balls.held[b]) balls.aim(b);
}

// The shield takes the hit and shatters into blue shards
function break_shield(x) {
    shield = false;
    fx.fracture(x - 6, SHIELD_Y, C_SHARDS, 0, false);
    fx.impact(fx.IMP_METAL, x, SHIELD_Y, C_SHIELD, 0, -1);
}

// ---- the laser --------------------------------------------------------------------

// Two shots, one from each end of the paddle
function fire() {
    if (!laser() || shot_wait > 0) return;
    shot(flr(paddle.left()) + 2);
    shot(flr(paddle.right()) - 3);
    shot_wait = SHOT_GAP;
    sfx(SFX.laser);
}

function shot(x) {
    for (var i = 0; i < SHOTS; i++)
        if (!shot_on[i]) {
            shot_on[i] = 1;
            shot_x[i] = x;
            shot_y[i] = PADDLE_Y - 4;
            return;
        }
}

// Shots fly up; one that meets a brick hits it and is gone
function update_shots() {
    for (var i = 0; i < SHOTS; i++) {
        if (!shot_on[i]) continue;
        shot_y[i] -= SHOT_SPEED;
        if (shot_y[i] < FIELD_T) {
            shot_on[i] = 0;
            continue;
        }
        var b = bricks.at(shot_x[i], shot_y[i]);
        if (b >= 0) {
            shot_on[i] = 0;
            bricks.hit(b, 1, shot_x[i], shot_y[i], 0, -1);
            fx.sparks(4, shot_x[i], shot_y[i] + 2, 0, 0.6, 1.0, 8, C_LASER);
        }
    }
}

// ---- drawing ------------------------------------------------------------------------

function draw() {
    // capsules: on the sheet at y 16, one 16-pixel slot each, in the order
    // of P_MULTI to P_SHIELD
    for (var i = 0; i < CAPSULES; i++)
        if (cap_on[i])
            sspr(cap_type[i] * 16, 16, 10, 6, cap_x[i], cap_y[i]);

    for (var i = 0; i < SHOTS; i++)
        if (shot_on[i]) {
            rectfill(shot_x[i], shot_y[i], shot_x[i], shot_y[i] + 3, C_LASER);
            pset(shot_x[i], shot_y[i], C_WHITE);
        }

    // the shield: a line that shimmers
    if (shield)
        line(FIELD_L, SHIELD_Y, FIELD_R - 1, SHIELD_Y,
             frame() % 6 < 3 ? C_SHIELD : 12);
}
