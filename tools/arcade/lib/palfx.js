// palfx: the palette as an effect -- fades, flashes and cycling. These
// change what the colours look like (setpal), so everything on the screen
// changes with them and nothing is redrawn.
//
//   palfx.keep()           remember palette entries 0-15 as they are: the
//                          colours fades and flashes go from and back to
//   palfx.fade(t, rgb)     every kept colour t of the way (0 to 1) to rgb:
//                          0x000000 fades to black, 0xffffff to white
//   palfx.flash(rgb)       every colour rgb at once, for a hit or a blast;
//                          palfx.fade(0, 0) puts them back
//   palfx.cycle(first, n, ticks)
//                          entries first to first + n - 1 shown turned one
//                          place every ticks ticks: water, lava, lights.
//                          Called each tick; it uses pal(c0, c1, 1)
//
// keep() is called by fade() and flash() the first time if the cart has
// not.

var kept = new Int32Array(16);
var have = false;

function keep() {
    for (var i = 0; i < 16; i++) kept[i] = getpal(i);
    have = true;
}

function mix(a, b, t) {
    var r = (a >> 16) & 255, g = (a >> 8) & 255, bl = a & 255;
    var r2 = (b >> 16) & 255, g2 = (b >> 8) & 255, b2 = b & 255;
    r = flr(r + (r2 - r) * t);
    g = flr(g + (g2 - g) * t);
    bl = flr(bl + (b2 - bl) * t);
    return (r << 16) | (g << 8) | bl;
}

function fade(t, rgb) {
    if (!have) keep();
    t = mid(0.0, t, 1.0);
    for (var i = 0; i < 16; i++) setpal(i, mix(kept[i], rgb, t));
}

function flash(rgb) {
    if (!have) keep();
    for (var i = 0; i < 16; i++) setpal(i, rgb);
}

function cycle(first, n, ticks) {
    if (n < 2) return;
    var turn = flr(frame() / max(1, ticks)) % n;
    for (var i = 0; i < n; i++) pal(first + i, first + (i + turn) % n, 1);
}
