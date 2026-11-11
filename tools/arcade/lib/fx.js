// fx: the feel of a hit -- a shake of the screen, a moment's stop -- and
// timers that count ticks down.
//
//   fx.shake(pixels, ticks)     the screen shaken up to pixels either way,
//                               for ticks; a bigger shake takes over
//   fx.stop(ticks)              a hit-stop: fx.stopped() true for ticks
//   fx.step()                   once a tick, first: counts them down
//   fx.stopped()                whether to leave the game still this tick
//   fx.view(x, y)               camera(x, y) with the shake added
//   fx.x, fx.y                  the shake this tick, for drawing by hand
//
//   fx.after(ticks, id)         id due in ticks, among up to 32 waiting
//   fx.due()                    the next id due this tick, or -1: call it
//                               until -1, after fx.step()
//
// A hit-stop is the game standing still for a tick or two after a big hit,
// so the hit reads: update() returns early while fx.stopped().
//
//   function update() {
//       fx.step();
//       if (fx.stopped()) return;
//       ...
//       if (hit) { fx.shake(2, 6); fx.stop(2); fx.after(30, RESPAWN); }
//       for (var id = fx.due(); id >= 0; id = fx.due()) happen(id);
//   }
//   function draw() { fx.view(0, 0); ... }

const TIMERS = 32;

var x = 0, y = 0;
var pixels = 0, shaking = 0, stopping = 0, still = false;
var left = new Int32Array(TIMERS), ids = new Int32Array(TIMERS);
var ready = new Int32Array(TIMERS), nready = 0;

function shake(px, ticks) {
    if (px >= pixels || shaking == 0) pixels = px;
    shaking = max(shaking, ticks);
}

function stop(ticks) {
    stopping = max(stopping, ticks);
}

function step() {
    still = stopping > 0;
    if (still) {
        stopping--;
        return;
    }
    if (shaking > 0) {
        shaking--;
        x = rnd(pixels * 2 + 1) - pixels;
        y = rnd(pixels * 2 + 1) - pixels;
    } else {
        x = 0;
        y = 0;
    }
    nready = 0;
    for (var i = 0; i < TIMERS; i++)
        if (left[i] > 0 && --left[i] == 0) ready[nready++] = ids[i];
}

function stopped() {
    return still;
}

function view(cx, cy) {
    camera(cx + x, cy + y);
}

function after(ticks, id) {
    for (var i = 0; i < TIMERS; i++)
        if (left[i] <= 0) {
            left[i] = max(1, ticks);
            ids[i] = id;
            return true;
        }
    return false;
}

function due() {
    return nready > 0 ? ready[--nready] : -1;
}
