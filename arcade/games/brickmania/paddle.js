// The paddle, kept by its centre x and its width w.
//
// Steering has weight: the wheel and the buttons push the paddle, and
// friction slows it when they stop, rather than it jumping to a speed.
// Its speed, vx, is also how it puts spin on the ball (balls.js).
//
// Spin steering: turning the wheel pushes it, the faster the harder.
// Dial steering: the finger's place on the wheel's top half, from nine
// o'clock round to three, is where the paddle goes.

const W_NORMAL = 24, W_WIDE = 36, W_NARROW = 14;

const WHEEL_PUSH = 0.35;        // speed added by each click of the wheel
const BUTTON_PUSH = 0.35;       // speed added each tick Prev or Next is held
const FRICTION = 0.75;          // speed kept each tick with nothing pushing
const TOP_SPEED = 4.5;          // the wheel and the buttons alike
const GROW = 1;                 // pixels a tick the width changes by

var x = 80.0, vx = 0.0;
var w = W_NORMAL, target_w = W_NORMAL;

function reset() {
    x = 80.0;
    vx = 0.0;
    reset_width();
}

function reset_width() {
    w = W_NORMAL;
    target_w = W_NORMAL;
}

function left() { return x - w / 2; }
function right() { return x + w / 2; }

// Expand and Shrink: the width eases to its new size over a few ticks
function set_width(width) {
    target_w = width;
}

function update() {
    var was = x;
    if (spin) steer_spin(); else steer_dial();

    // kept inside the walls
    var lo = FIELD_L + w / 2, hi = FIELD_R - w / 2;
    if (x < lo || x > hi) {
        x = mid(lo, x, hi);
        vx = 0.0;
    }
    if (!spin) vx = x - was;    // Dial: its speed is how far it went

    grow();
}

function steer_spin() {
    var pushed = false;
    var clicks = wheel();
    if (clicks != 0) {
        vx += clicks * WHEEL_PUSH;
        pushed = true;
    }
    if (btn(0)) { vx -= BUTTON_PUSH; pushed = true; }
    if (btn(1)) { vx += BUTTON_PUSH; pushed = true; }
    if (!pushed) vx *= FRICTION;
    vx = mid(-TOP_SPEED, vx, TOP_SPEED);
    if (abs(vx) < 0.05) vx = 0.0;
    x += vx;
}

function steer_dial() {
    wheel();                    // the turn is not used: read it off
    var p = wheelpos();
    // the top half of the wheel: 72 (nine o'clock) through 0 to 24
    if (p >= 72 || (p >= 0 && p <= 24)) {
        var along = (p >= 72 ? p - 72 : p + 24) / 48.0;    // 0 to 1
        var want = FIELD_L + w / 2 + along * (FIELD_R - FIELD_L - w);
        x += (want - x) * 0.5;  // eased, so a jump of the finger is not
    }                           // a jump of the paddle
    if (btn(0)) x -= 3;
    if (btn(1)) x += 3;
}

// The width changing a pixel a tick, with sparks off the ends: cool ones
// growing, warm ones shrinking
function grow() {
    if (w == target_w) return;
    var growing = target_w > w;
    w += growing ? GROW : -GROW;
    var c = growing ? 13 : 9;
    fx.sparks(1, left(), PADDLE_Y + 1, -0.6, -0.3, 0.4, 10, c);
    fx.sparks(1, right(), PADDLE_Y + 1, 0.6, -0.3, 0.4, 10, c);
}

// The paddle's picture is 24 x 4 on the sheet. Its three pixels at each
// end are drawn as they are; the middle is stretched to the width.
function draw() {
    var sx = 0;
    if (powers.laser()) sx = SHEET_PADDLE_LASER;
    else if (powers.sticky()) sx = SHEET_PADDLE_STICKY;
    var px = flr(left()), sy = SHEET_PADDLE_Y;
    sspr(sx, sy, 3, 4, px, PADDLE_Y);
    sspr(sx + 3, sy, 18, 4, px + 3, PADDLE_Y, w - 6, 4);
    sspr(sx + 21, sy, 3, 4, px + w - 3, PADDLE_Y);
}
