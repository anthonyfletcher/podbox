// tween: easing curves, and values moved along them over a number of
// ticks.
//
// A curve takes t from 0 to 1 and gives how far along to be, 0 to 1:
//
//   tween.linear  in_quad  out_quad  in_out_quad  out_cubic  out_back
//   out_bounce
//
// tween.start(arr, i, target, ticks, curve) moves arr[i] -- any slot of an
// array or typed array -- from where it is to target over ticks, along
// curve (tween.out_quad, say: the curves are values). One already running
// on the same slot is replaced. tween.step() advances them all, once a
// tick, and returns how many are still running; tween.running(arr, i)
// says whether that slot is moving. Up to 32 run at once.

const MAX = 32;

var arrs = [];
var curves = [];
var slot = new Int32Array(MAX);
var begin = [];
var finish = [];
var length = new Int32Array(MAX);
var at = new Int32Array(MAX);
var live = new Uint8Array(MAX);

function linear(t) { return t; }

function in_quad(t) { return t * t; }

function out_quad(t) { return t * (2.0 - t); }

function in_out_quad(t) {
    if (t < 0.5) return 2.0 * t * t;
    return -1.0 + (4.0 - 2.0 * t) * t;
}

function out_cubic(t) {
    var u = t - 1.0;
    return u * u * u + 1.0;
}

// past the target a little, and back
function out_back(t) {
    var u = t - 1.0;
    return 1.0 + u * u * (2.7 * u + 1.7);
}

function out_bounce(t) {
    if (t < 0.3637) return 7.5625 * t * t;
    if (t < 0.7273) { t -= 0.5455; return 7.5625 * t * t + 0.75; }
    if (t < 0.9091) { t -= 0.8182; return 7.5625 * t * t + 0.9375; }
    t -= 0.9546;
    return 7.5625 * t * t + 0.9844;
}

function find(arr, i) {
    for (var k = 0; k < MAX; k++)
        if (live[k] && arrs[k] == arr && slot[k] == i) return k;
    return -1;
}

function start(arr, i, target, ticks, curve) {
    if (arrs.length == 0)
        for (var j = 0; j < MAX; j++) { arrs.push(null); curves.push(null); begin.push(0); finish.push(0); }
    var k = find(arr, i);
    if (k < 0)
        for (k = 0; k < MAX && live[k]; k++) {}
    if (k >= MAX) return -1;
    arrs[k] = arr;
    curves[k] = curve;
    slot[k] = i;
    begin[k] = arr[i];
    finish[k] = target;
    length[k] = max(1, ticks);
    at[k] = 0;
    live[k] = 1;
    return k;
}

function step() {
    var n = 0;
    for (var k = 0; k < MAX; k++) {
        if (!live[k]) continue;
        at[k]++;
        var f = curves[k];
        var t = at[k] >= length[k] ? 1.0 : at[k] / (length[k] + 0.0);
        arrs[k][slot[k]] = begin[k] + (finish[k] - begin[k]) * f(t);
        if (at[k] >= length[k]) {
            arrs[k][slot[k]] = finish[k];
            live[k] = 0;
            arrs[k] = null;
        } else
            n++;
    }
    return n;
}

function running(arr, i) {
    return find(arr, i) >= 0;
}
