// Input Test: what the wheel and the buttons tell a cart, drawn so it can
// be checked by eye on a player -- and a launch counter, to show saving.

var runs = 0;
var spun = 0;            // wheel() summed: 96 a turn
var paddle = 64.0;
var presses = new Int32Array(5);
var names = ["PREV", "NEXT", "MENU", "PLAY", "SEL"];
var shown = 1;

// Pause-menu items: one relabels itself and keeps the menu open, one
// acts and resumes.
function toggle(b) {
    shown = 1 - shown;
    menuitem(1, shown ? "Counts: shown" : "Counts: hidden", toggle);
    return true;
}

function reset(b) {
    for (var i = 0; i < 5; i++) presses[i] = 0;
    spun = 0;
}

function init() {
    runs = dget(0) + 1;
    dset(0, runs);
    menuitem(1, "Counts: shown", toggle);
    menuitem(2, "Reset counts", reset);
}

function update() {
    var d = wheel();
    spun += d;
    paddle = mid(8.0, paddle + d * 0.75, 120.0);
    for (var i = 0; i < 5; i++)
        if (btnp(i)) presses[i]++;
}

function draw() {
    cls(1);
    print("INPUT TEST", 60, 2, 7);

    // The finger's place: a ring, 0 at the top, clockwise.
    var p = wheelpos();
    circ(32, 40, 22, 5);
    if (p >= 0) {
        var a = p / 96.0;
        var x = 32 + sin(a) * -22, y = 40 - cos(a) * 22;
        circfill(x, y, 3, 10);
    }
    print("POS " + p, 14, 66, 6);

    // The wheel's clicks: a total, and a paddle they move.
    print("SPUN " + spun, 70, 20, 6);
    print("TURNS " + flr(spun / 96.0), 70, 28, 6);
    rectfill(paddle - 8, 100, paddle + 8, 102, 11);

    // Each button: lit while held, with its press count.
    for (var i = 0; i < 5; i++) {
        var y = 40 + i * 9;
        rectfill(70, y, 76, y + 6, btn(i) ? 8 : 5);
        print(shown ? names[i] + " " + presses[i] : names[i], 80, y + 1, 7);
    }

    print("RUN " + runs + "  TICK " + frame(), 2, 110, 13);

    // Two bars down the right: one growing with the ticks, one the
    // finger's place on the wheel.
    rectfill(140, 4, 147, 4 + (frame() % 112), 14);
    rectfill(150, 116 - p, 157, 116, 12);
}
