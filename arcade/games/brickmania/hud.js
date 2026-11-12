// The strip along the top: the score at the left, the lives at the right
// as balls, and between them the power-ups that are running, each its
// capsule with a bar for the time it has left. It is drawn after the
// camera is put back, so it never shakes.

function draw() {
    rectfill(0, 0, 159, HUD_H - 1, 0);
    line(0, HUD_H - 1, 159, HUD_H - 1, C_FRAME);

    print(pad(points, 6), 2, 2, C_WHITE);
    if (powers.shrunk()) print("X2", 30, 2, 9);     // Shrink's bonus

    // lives: a ball each, the newest at the right
    for (var i = 0; i < lives && i < 8; i++)
        sspr(0, SHEET_BALL_Y, 4, 4, 154 - i * 6, 2);

    // the timed power-ups
    var x = 52;
    x = timer(x, powers.P_FIRE, powers.fire_ticks);
    x = timer(x, powers.P_LASER, powers.laser_ticks);
    x = timer(x, powers.P_STICKY, powers.sticky_ticks);
    x = timer(x, powers.P_SLOW, powers.slow_ticks);
    if (powers.shielded()) sspr(powers.P_SHIELD * 16, 16, 10, 6, x, 1);
}

// One running power-up at x: its capsule, and a bar under it that empties
// as it runs out. Returns where the next one goes.
function timer(x, type, ticks) {
    if (ticks <= 0) return x;
    sspr(type * 16, 16, 10, 6, x, 1);
    var bar = flr(10 * ticks / powers.TIMED);
    if (bar > 0) line(x, 7, x + bar - 1, 7, powers.COLOURS[type]);
    return x + 13;
}

// n as text, with zeros in front to make `digits` figures
function pad(n, digits) {
    var s = "" + n;
    while (s.length < digits) s = "0" + s;
    return s;
}
