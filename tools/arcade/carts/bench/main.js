// Bench: fixed workloads, one phase at a time, each announced with
// printh("phase <name>") so a measuring shell can time them. Each phase
// runs PHASE ticks; then the cart quits.

const PHASE = 75;
var names = ["idle", "loop", "fix", "fields", "array", "calls", "native",
             "strings", "cls", "rectfill", "circfill", "line", "spr",
             "print", "map", "pset", "tline", "palette"];
var phase = -1, ticks = PHASE;
var objs = [];
var grid = new Int32Array(2048);
var sink = 0;

function init() {
    for (var i = 0; i < 100; i++)
        objs.push({ x: i, y: i * 2, vx: 1, vy: -1 });
    // Sprite 1: a striped block; the map: sprites 1 and 2 in a checker.
    for (var y = 0; y < 8; y++)
        for (var x = 0; x < 8; x++) {
            sset(8 + x, y, 8 + (x + y) % 8);
            sset(16 + x, y, x == 0 || y == 0 ? 0 : 12);
        }
    for (var my = 0; my < 32; my++)
        for (var mx = 0; mx < 32; mx++)
            mset(mx, my, 1 + (mx + my) % 2);
}

function add(a, b) { return a + b; }

function update() {
    if (++ticks > PHASE) {
        ticks = 1;
        phase++;
        if (phase >= names.length) {
            printh("done");
            quit();
            return;
        }
        printh("phase " + names[phase]);
    }
    var i, s = 0, n = names[phase];
    if (n == "loop") {
        for (i = 0; i < 2000; i++) s += i;
    } else if (n == "fix") {
        var f = 1.0;
        for (i = 0; i < 1000; i++) f = f * 1.001 + 0.25;
        s = flr(f);
    } else if (n == "fields") {
        for (var k = 0; k < 10; k++)
            for (i = 0; i < 100; i++) {
                var o = objs[i];
                o.x += o.vx;
                o.y += o.vy;
            }
    } else if (n == "array") {
        for (i = 1; i < 2048; i++) grid[i] = grid[i - 1] + 1;
    } else if (n == "calls") {
        for (i = 0; i < 1000; i++) s = add(s, i);
    } else if (n == "native") {
        for (i = 0; i < 1000; i++) s += abs(i);
    } else if (n == "strings") {
        var t = "";
        for (i = 0; i < 200; i++) t = "s" + i;
        s = t.length;
    }
    sink = s;
}

function draw() {
    if (phase < 0 || phase >= names.length) return;
    var i, n = names[phase];
    if (n == "cls") {
        cls(ticks % 16);
    } else if (n == "rectfill") {
        for (i = 0; i < 20; i++) rectfill(i * 4, i * 4, i * 4 + 31, i * 4 + 31, i % 16);
    } else if (n == "circfill") {
        for (i = 0; i < 20; i++) circfill(16 + i * 5, 60, 12, i % 16);
    } else if (n == "line") {
        for (i = 0; i < 100; i++) line(0, i, 127, 119 - i, i % 16);
    } else if (n == "spr") {
        for (i = 0; i < 200; i++) spr(1, (i * 13) % 120, (i * 7) % 112);
    } else if (n == "print") {
        for (i = 0; i < 20; i++) print("THE QUICK BROWN FOX 0123", 0, i * 6, 7);
    } else if (n == "map") {
        map(0, 0, 0, 0, 16, 15);
    } else if (n == "pset") {
        for (i = 0; i < 2000; i++) pset(i % 128, (i >> 7) + ticks % 100, i % 16);
    } else if (n == "tline") {
        for (i = 0; i < 120; i++) tline(0, i, 127, i, 0, i / 8.0, 0.0625, 0);
    } else if (n == "palette") {
        setpal(0, 0x000000);
    }
}
