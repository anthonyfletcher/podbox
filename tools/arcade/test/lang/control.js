// Control flow, calls, recursion, function values.
var calls = 0;
var ops = [add, sub, function (a, b) { return a * b; }];

function add(a, b) { calls++; return a + b; }
function sub(a, b) { calls++; return a - b; }
function noret() { calls++; }
function args(a, b, c) { return a + "," + b + "," + c; }
function ack(m, n) {
    if (m == 0) return n + 1;
    if (n == 0) return ack(m - 1, 1);
    return ack(m - 1, ack(m, n - 1));
}
function collatz(n) {
    var steps = 0;
    while (n != 1) {
        n = n % 2 == 0 ? n / 2 : 3 * n + 1;
        steps++;
    }
    return steps;
}
function classify(n) {
    switch (n % 5) {
    case 0: return "zero";
    case 1:
    case 2: return "low";
    case add(1, 2): return "three";
    default: return "high";
    }
}

function init() {
    var i, j, s = "";
    for (i = 0; i < 10; i++) {
        if (i == 2) continue;
        if (i == 7) break;
        s = s + i;
    }
    printh("for " + s);
    i = 0; s = "";
    do { s = s + i; i += 3; } while (i < 20);
    printh("do " + s);
    s = "";
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            if (j > i) break;
            s = s + i + j + " ";
        }
    printh("nested " + s);
    for (i = 0; i < 12; i++) s = s + classify(i) + " ";
    printh("switch " + s);
    var fall = 0;
    switch (3) { case 1: fall += 1; case 3: fall += 3; case 4: fall += 4; break; case 5: fall += 5; }
    printh("fall " + fall);
    switch (9) { case 1: fall = 0; }
    printh("nomatch " + fall);
    printh("ops " + ops[0](3, 4) + " " + ops[1](3, 4) + " " + ops[2](3, 4));
    printh("args " + args(1) + " " + args(1, 2, 3, 4) + " " + noret());
    var f = ops[1];
    printh("fnval " + f(10, 1, 99) + " " + typeof f);
    printh("ack " + ack(2, 3) + " collatz " + collatz(27));
    printh("logic " + (0 && 5) + " " + (3 && 5) + " " + (0 || 5) + " " + (3 || 5) +
           " " + (null || "d") + " " + ("" || 0) + " " + (1 && null));
    var t = 0;
    (t > 0) && add(1, 1);
    (t > 0) || add(1, 1);
    printh("calls " + calls);
    printh("ternary " + (t ? "a" : t + 1 ? "b" : "c"));
    var k = 0;
    while (true) { k++; if (k > 5) break; }
    for (;;) { k++; if (k > 9) break; }
    printh("loops " + k);
    printh("seq " + (k = 1, k + 1));
    var r = function named(n) { return n < 2 ? 1 : n * named(n - 1); };
    printh("named " + r(6));
}

var ticks = 0;
function update() {
    ticks++;
    if (ticks % 2 == 0) printh("tick " + ticks);
}
