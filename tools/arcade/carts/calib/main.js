// Calib: one native or one operation per phase, called n times a tick
// with constant arguments, beside an empty loop of the same shape, so a
// measuring shell can price each call. Each phase is announced with
// printh("phase <name>") and runs PHASE ticks; then the cart quits.

const PHASE = 30;
var phases = [
    ["empty", 1000], ["pset", 1000], ["pget", 1000], ["line_h", 200],
    ["line_d", 200], ["line_short", 1000], ["rect", 200], ["rectfill_s", 500],
    ["rectfill_l", 30], ["cls", 30], ["circfill_s", 300], ["circfill_l", 30],
    ["circ_l", 100], ["trifill", 100], ["spr", 500], ["spr_clip", 500],
    ["spr_flip", 500], ["spr_big", 100], ["sspr_scale", 100], ["map", 20],
    ["print_1", 500], ["print_20", 100], ["print_big", 100], ["tline", 100],
    ["blit", 100], ["fillp", 30], ["mod", 1000], ["div", 1000],
    ["fixmul", 1000], ["concat", 200], ["push", 500], ["field", 1000],
    ["call", 1000], ["native", 1000], ["index", 1000], ["view", 1000],
    ["copy", 100], ["fill", 30]
];
var phase = -1, ticks = PHASE;
var scr, buf, grid, picture, rec, list, sink = 0;

function init() {
    scr = mem(MEM.SCREEN);
    buf = new Uint8Array(1600);
    grid = new Int32Array(1024);
    picture = img(32, 32);
    target(picture);
    rectfill(0, 0, 31, 31, 9);
    circfill(16, 16, 10, 0);
    target(SCREEN);
    rec = { x: 0, y: 0 };
    for (var y = 0; y < 8; y++)
        for (var x = 0; x < 8; x++)
            sset(8 + x, y, x == 0 || y == 0 ? 0 : 8 + (x + y) % 8);
    for (var my = 0; my < 32; my++)
        for (var mx = 0; mx < 32; mx++)
            mset(mx, my, 1);
}

function add(a, b) { return a + b; }

function update() {
    if (phase >= phases.length) return;
    if (++ticks > PHASE) {
        ticks = 1;
        phase++;
        if (phase >= phases.length) {
            printh("done");
            quit();
            return;
        }
        printh("phase " + phases[phase][0]);
    }
    var i, s = 0, n = phases[phase][1], name = phases[phase][0];
    var f = 1.0, t = "";
    if (name == "mod") {
        for (i = 0; i < n; i++) s = i % 7;
    } else if (name == "div") {
        for (i = 0; i < n; i++) s = i / 7;
    } else if (name == "fixmul") {
        for (i = 0; i < n; i++) f = f * 1.001;
    } else if (name == "concat") {
        for (i = 0; i < n; i++) t = "s" + i;
    } else if (name == "push") {
        list = [];
        for (i = 0; i < n; i++) list.push(i);
    } else if (name == "field") {
        for (i = 0; i < n; i++) rec.x += 1;
    } else if (name == "call") {
        for (i = 0; i < n; i++) s = add(s, i);
    } else if (name == "native") {
        for (i = 0; i < n; i++) s = abs(i);
    } else if (name == "index") {
        for (i = 0; i < n; i++) grid[i & 1023] = i;
    } else if (name == "empty") {
        for (i = 0; i < n; i++) {}
    }
    sink = s + flr(f) + t.length;
}

function draw() {
    if (phase < 0 || phase >= phases.length) return;
    var i, n = phases[phase][1], name = phases[phase][0];
    if (name == "pset") for (i = 0; i < n; i++) pset(50, 50, 7);
    else if (name == "pget") for (i = 0; i < n; i++) sink = pget(50, 50);
    else if (name == "line_h") for (i = 0; i < n; i++) line(0, 10, 159, 10, 7);
    else if (name == "line_d") for (i = 0; i < n; i++) line(0, 0, 119, 119, 7);
    else if (name == "line_short") for (i = 0; i < n; i++) line(10, 10, 13, 12, 7);
    else if (name == "rect") for (i = 0; i < n; i++) rect(10, 10, 60, 60, 7);
    else if (name == "rectfill_s") for (i = 0; i < n; i++) rectfill(10, 10, 17, 17, 7);
    else if (name == "rectfill_l") for (i = 0; i < n; i++) rectfill(0, 0, 159, 119, 7);
    else if (name == "cls") for (i = 0; i < n; i++) cls(1);
    else if (name == "circfill_s") for (i = 0; i < n; i++) circfill(50, 50, 4, 7);
    else if (name == "circfill_l") for (i = 0; i < n; i++) circfill(80, 60, 50, 7);
    else if (name == "circ_l") for (i = 0; i < n; i++) circ(80, 60, 50, 7);
    else if (name == "trifill") for (i = 0; i < n; i++) trifill(10, 10, 150, 30, 60, 110, 7);
    else if (name == "spr") for (i = 0; i < n; i++) spr(1, 40, 40);
    else if (name == "spr_clip") for (i = 0; i < n; i++) spr(1, -4, 40);
    else if (name == "spr_flip") for (i = 0; i < n; i++) spr(1, 40, 40, 1, 1, true);
    else if (name == "spr_big") for (i = 0; i < n; i++) spr(1, 40, 40, 4, 4);
    else if (name == "sspr_scale") for (i = 0; i < n; i++) sspr(8, 0, 8, 8, 10, 10, 48, 48);
    else if (name == "map") for (i = 0; i < n; i++) map(0, 0, 0, 0, 20, 15);
    else if (name == "print_1") for (i = 0; i < n; i++) print("A", 10, 10, 7);
    else if (name == "print_20") for (i = 0; i < n; i++) print("ABCDEFGHIJKLMNOPQRST", 0, 10, 7);
    else if (name == "print_big") {
        font(FONT.BIG);
        for (i = 0; i < n; i++) print("ABCDEFGHIJKLMNOPQRST", 0, 10, 7);
        font(FONT.SMALL);
    }
    else if (name == "tline") for (i = 0; i < n; i++) tline(0, 50, 159, 50, 0, 0, 0.125, 0);
    else if (name == "blit") for (i = 0; i < n; i++) blit(picture, 10, 10);
    else if (name == "fillp") {
        fillp(0x5a5a);
        for (i = 0; i < n; i++) rectfill(0, 0, 159, 119, 7 + 256 * 8);
        fillp();
    }
    else if (name == "view") for (i = 0; i < n; i++) scr[i] = 7;
    else if (name == "copy") for (i = 0; i < n; i++) copy(scr, 0, buf, 0, 1600);
    else if (name == "fill") for (i = 0; i < n; i++) fill(scr, 7);
}
