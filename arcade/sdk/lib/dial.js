// dial: the click wheel as a control.
//
// wheel() is the clicks turned this tick, 96 to a turn, and wheelpos()
// where the finger is (0-95 clockwise from the top, -1 untouched). These
// keep what a game usually wants of them:
//
//   dial.steps(n)             whole detents of n to a turn this tick, the
//                             rest carried to the next: a knob or a setting
//                             of n positions, the same count however fast it
//                             turns. For a list, wheelsteps() moves as the
//                             player's own lists do
//   dial.angle()              the turns turned since the cart began
//   dial.speed()              clicks a tick, smoothed: how fast it spins
//   dial.paddle(lo, hi, from, to)
//                             the finger's place on an arc of the wheel,
//                             from position `from` clockwise to `to`, as
//                             lo to hi; -1 off the arc or untouched
//
// angle() and speed() follow the wheel once a tick however often they are
// asked. steps() keeps one carry, for the one detent count a cart uses.

var carry = 0;
var turned = 0.0;
var smooth = 0.0;
var seen = -1;

// once a tick: the turns and the smoothed speed
function follow() {
    if (frame() == seen) return;
    seen = frame();
    var w = wheel();
    turned += w / 96.0;
    smooth += (w - smooth) / 4.0;
}

function steps(n) {
    carry += wheel() * n;
    var s = carry >= 0 ? flr(carry / 96) : -flr(-carry / 96);
    carry -= s * 96;
    return s;
}

function angle() {
    follow();
    return turned;
}

function speed() {
    follow();
    return smooth;
}

function paddle(lo, hi, from, to) {
    var p = wheelpos();
    if (p < 0) return -1;
    var along = (p - from + 96) % 96, len = (to - from + 96) % 96;
    if (along > len || len == 0) return -1;
    return lo + (hi - lo) * along / (len + 0.0);
}
