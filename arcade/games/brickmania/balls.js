// The balls: up to four at once, each 4 x 4 pixels, kept by its centre:
// a ball at x covers x - 2 to x + 1, and the same down.
//
// A ball moves in small steps each tick, so that even at full speed it
// cannot pass through a brick or the paddle between one tick and the
// next. Its speed rises a little with every brick broken. Each ball
// remembers where it has been, for its trail.

const MOST = 4;                 // balls at once

const START_SPEED = 1.5;        // pixels a tick, at the first serve
const LAP_SPEED = 0.15;         // and this much more each time round the levels
const SPEED_UP = 0.015;         // faster by this for every brick broken
const TOP_SPEED = 3.5;
const MIN_DOWN = 0.6;           // the least it moves up or down a tick, so it
                                // never skims sideways for ever
const MIN_ACROSS = 0.25;        // and across, so it never bounces straight
                                // up and down on one spot
const STEP = 1.5;               // no step longer than this

const TRAIL = 12;               // positions kept for each ball's trail

var on = zeros(MOST);           // 1 while a ball is in play
var x = zeros(MOST), y = zeros(MOST), vx = zeros(MOST), vy = zeros(MOST);
var held = zeros(MOST);         // 1 while it sits on the paddle
var held_dx = zeros(MOST);      // where along the paddle, from its centre
var held_ticks = zeros(MOST);   // how long it has been held there
var trail_x = zeros(MOST * TRAIL), trail_y = zeros(MOST * TRAIL);
                                // TRAIL positions a ball, newest first
var speed = START_SPEED, start_speed = START_SPEED;

// n zeros that hold fractions
function zeros(n) {
    return new FixArray(n);
}

// A caught ball (Sticky) lets go by itself after this long
const HOLD_MOST = 99;

// Trail colours, brightest first: plain, several balls (gold), fireball,
// slow, sticky
var TRAIL_PLAIN = [6, 13, 1, 1];
var TRAIL_MULTI = [56, 10, 9, 4];
var TRAIL_FIRE = [10, 9, 55, 2];
var TRAIL_SLOW = [57, 28, 12, 1];
var TRAIL_STICKY = [58, 11, 3, 3];

// ---- making and losing balls ------------------------------------------------

function reset() {
    for (var b = 0; b < MOST; b++) on[b] = false;
    speed = start_speed;
}

// The start of a level: how many times round the levels have come
function set_start_speed(lap) {
    start_speed = START_SPEED + lap * LAP_SPEED;
    speed = start_speed;
}

// A new ball at bx, by moving dvx, dvy: its number, or -1 with four out
function add(bx, by, dvx, dvy) {
    for (var b = 0; b < MOST; b++)
        if (!on[b]) {
            on[b] = true;
            held[b] = false;
            x[b] = bx; y[b] = by; vx[b] = dvx; vy[b] = dvy;
            for (var k = 0; k < TRAIL; k++) {
                trail_x[b * TRAIL + k] = bx;
                trail_y[b * TRAIL + k] = by;
            }
            return b;
        }
    return -1;
}

// Ball b sits on the paddle where it touched it, until let go
function hold(b) {
    if (b < 0) return;
    held[b] = true;
    held_ticks[b] = 0;
    held_dx[b] = mid(-paddle.w / 2 + 2, x[b] - paddle.x, paddle.w / 2 - 2);
    vx[b] = 0.0;
    vy[b] = 0.0;
}

// Every held ball let go, up and a little the way the paddle moves; true
// when there was one
function release() {
    var any = false;
    for (var b = 0; b < MOST; b++)
        if (on[b] && held[b]) {
            held[b] = false;
            vx[b] = held_dx[b] / 8.0 + paddle.vx * 0.25;
            if (abs(vx[b]) < MIN_ACROSS)        // served still: a side at random
                vx[b] = rnd(2) == 0 ? -0.5 : 0.5;
            vy[b] = -1.0;
            aim(b);
            any = true;
        }
    return any;
}

function count() {
    var n = 0;
    for (var b = 0; b < MOST; b++) if (on[b]) n++;
    return n;
}

// Faster for a broken brick, up to the top speed
function speed_up() {
    speed = min(speed + SPEED_UP, TOP_SPEED);
}

// The speed balls move at now: Slow takes off 40%
function current_speed() {
    return powers.slowed() ? speed * 0.6 : speed;
}

// Multiball: each ball in play becomes three, fanned apart, up to four in all
function split() {
    var n = MOST;
    for (var b = 0; b < n; b++) {
        if (!on[b] || held[b]) continue;
        for (var side = -1; side <= 1; side += 2) {
            var nb = add(x[b], y[b], vx[b] + side * 0.8, vy[b]);
            if (nb >= 0) aim(nb);
        }
    }
}

// ---- aiming -------------------------------------------------------------------

// Ball b's speed set to the current speed, keeping its direction -- but
// never so flat that it barely moves up or down, nor so steep that it
// barely moves across
function aim(b) {
    var s = current_speed();
    var len = sqrt(vx[b] * vx[b] + vy[b] * vy[b]);
    if (len < 0.01) {
        vx[b] = 0.0;
        vy[b] = -s;
        return;
    }
    vx[b] = vx[b] * s / len;
    vy[b] = vy[b] * s / len;
    if (abs(vy[b]) < MIN_DOWN) {
        vy[b] = sgn(vy[b]) * MIN_DOWN;
        vx[b] = sgn(vx[b]) * sqrt(max(s * s - MIN_DOWN * MIN_DOWN, 0.0));
    }
    if (abs(vx[b]) < MIN_ACROSS) {
        vx[b] = sgn(vx[b]) * MIN_ACROSS;
        vy[b] = sgn(vy[b]) * sqrt(max(s * s - MIN_ACROSS * MIN_ACROSS, 0.0));
    }
}

// ---- every tick ---------------------------------------------------------------

function update() {
    for (var b = 0; b < MOST; b++) {
        if (!on[b]) continue;
        remember(b);
        if (held[b]) {
            x[b] = paddle.x + held_dx[b];
            y[b] = PADDLE_Y - 2;
            held_ticks[b]++;
            // a caught ball goes by itself in the end; a served one waits
            if (state == ST_PLAY && held_ticks[b] > HOLD_MOST) release();
            continue;
        }
        // In steps no longer than STEP, stopping early if the ball is lost
        // or caught
        var steps = flr(current_speed() / STEP) + 1;
        for (var s = 0; s < steps && on[b] && !held[b]; s++)
            step(b, vx[b] / steps, vy[b] / steps);
    }
    if (count() == 0 && state == ST_PLAY) ball_lost();
}

// Ball b's newest position at the front of its trail
function remember(b) {
    var base = b * TRAIL;
    for (var k = TRAIL - 1; k > 0; k--) {
        trail_x[base + k] = trail_x[base + k - 1];
        trail_y[base + k] = trail_y[base + k - 1];
    }
    trail_x[base] = x[b];
    trail_y[base] = y[b];
}

// One small step of ball b, and whatever it meets on the way: walls, then
// the paddle, the shield, the bottom, and bricks
function step(b, dx, dy) {
    var ox = x[b], oy = y[b];
    x[b] += dx;
    y[b] += dy;

    // the walls and the ceiling
    if (x[b] - 2 < FIELD_L) bounce_wall(b, FIELD_L + 2, 1);
    if (x[b] + 1 >= FIELD_R) bounce_wall(b, FIELD_R - 2, -1);
    if (y[b] - 2 < FIELD_T) {
        y[b] = FIELD_T + 2;
        vy[b] = abs(vy[b]);
        fx.impact(fx.IMP_WALL, x[b], FIELD_T, C_GREY, 0, 1);
    }

    // the paddle: crossed from above, within its ends
    if (vy[b] > 0 && oy + 1 < PADDLE_Y && y[b] + 1 >= PADDLE_Y &&
        x[b] + 1 >= paddle.left() && x[b] - 2 <= paddle.right()) {
        hit_paddle(b);
        return;
    }

    // the shield, once
    if (vy[b] > 0 && powers.shielded() && oy + 1 < SHIELD_Y && y[b] + 1 >= SHIELD_Y) {
        y[b] = SHIELD_Y - 2;
        vy[b] = -abs(vy[b]);
        powers.break_shield(x[b]);
        return;
    }

    // out of the bottom
    if (y[b] > FIELD_B + 2) {
        on[b] = false;
        return;
    }

    hit_bricks(b, ox, oy);
}

function bounce_wall(b, at, way) {
    x[b] = at;
    vx[b] = way * abs(vx[b]);
    fx.impact(fx.IMP_WALL, way > 0 ? FIELD_L : FIELD_R - 1, y[b], C_GREY, way, 0);
}

// Off the paddle: the further from its centre, the more it angles out,
// and the paddle's own movement adds to it. A paddle touch ends a combo.
function hit_paddle(b) {
    y[b] = PADDLE_Y - 2;
    var offset = (x[b] - paddle.x) / (paddle.w / 2.0);
    vx[b] += offset * 1.2 + paddle.vx * 0.25;
    vy[b] = -abs(vy[b]);
    aim(b);
    combo_reset();
    fx.impact(fx.IMP_PADDLE, x[b], PADDLE_Y, C_WHITE, 0, -1);
    if (powers.sticky()) {
        hold(b);
        fx.sparks(6, x[b], PADDLE_Y, 0, -0.5, 0.8, 12, C_STICKY);
    }
}

// The brick under the ball's centre, if any. Coming in from the side, the
// ball turns back across; from above or below, it turns back up or down.
// A fireball goes straight through anything that breaks.
function hit_bricks(b, ox, oy) {
    var i = bricks.at(x[b], y[b]);
    if (i < 0) return;
    var fire = powers.fireball();
    var damage = fire ? 3 : 1;
    var broke = bricks.hit(i, damage, x[b], y[b], vx[b], vy[b]);
    if (fire && !bricks.is_metal(i)) {
        fx.impact(fx.IMP_POWERED, x[b], y[b], C_FIRE, vx[b], vy[b]);
        return;
    }
    // which side it came in from: the row it was in a step ago
    var r = bricks.row_at(y[b]);
    if (bricks.row_at(oy) == r) {
        vx[b] = -vx[b];
        x[b] = ox;
    } else {
        vy[b] = -vy[b];
        y[b] = oy;
    }
}

// ---- drawing ------------------------------------------------------------------

function draw() {
    var multi = count() > 1;
    for (var b = 0; b < MOST; b++) {
        if (!on[b]) continue;
        draw_trail(b, multi);
        sspr(sprite_x(b), SHEET_BALL_Y, 4, 4, x[b] - 2, y[b] - 2);
    }
}

// Which ball on the sheet: normal, fire, sticky or slow
function sprite_x(b) {
    if (powers.fireball()) return 8;
    if (held[b] && state == ST_PLAY) return 16;
    if (powers.slowed()) return 24;
    return 0;
}

// The trail: older points smaller and darker. Powered balls and long
// combos have longer trails.
function draw_trail(b, multi) {
    if (held[b]) return;
    var ramp = TRAIL_PLAIN, length = 5;
    if (powers.fireball()) { ramp = TRAIL_FIRE; length = 10; }
    else if (powers.slowed()) { ramp = TRAIL_SLOW; length = 11; }
    else if (powers.sticky()) { ramp = TRAIL_STICKY; length = 7; }
    else if (multi) { ramp = TRAIL_MULTI; length = 8; }
    length = min(length + flr(combo / 2), TRAIL - 1);

    var base = b * TRAIL;
    for (var k = length; k >= 1; k--) {
        var c = ramp[flr(k * 4 / (length + 1))];
        var tx = trail_x[base + k], ty = trail_y[base + k];
        if (k <= length / 3) rectfill(tx - 1, ty - 1, tx, ty, c);   // 2 x 2
        else pset(tx, ty, c);                                       // a dot
    }
}
