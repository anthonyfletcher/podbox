// Fractions: 16.16 values written with a point, mixed with integers.
const HALF = 0.5;
var vals = [0.0, 1.0, -1.0, 0.5, -0.5, 1.5, -2.75, 0.0001, 3.14159, 100.25,
            32767.5, -32768.0, 2, -3, 7, 0, 40000];

function init() {
    var i, j, a, b;
    for (i = 0; i < vals.length; i++) {
        a = vals[i];
        printh("v " + a + " " + -a + " " + Math.floor(a) + " " + Math.ceil(a) +
               " " + Math.round(a) + " " + Math.trunc(a) + " " + Math.abs(a) +
               " " + Math.sign(a) + " " + !a + " " + (a | 0) + " " + ~a);
        for (j = 0; j < vals.length; j++) {
            b = vals[j];
            var line = a + " " + b + ": " + (a + b) + " " + (a - b) + " " +
                (a * b) + " " + (a < b) + (a <= b) + (a > b) + (a >= b) +
                (a == b) + (a != b) + " " + (a & b) + " " + (a << 2);
            if (b != 0) line = line + " " + (a / b) + " " + (a % b);
            printh(line);
        }
    }
    printh("const " + HALF + " " + (HALF * 3) + " " + (1 / 3.0) + " " + (2.0 / 3) +
           " " + (10 / 4) + " " + (10 / 4.0) + " " + deg(90) + " " + deg(-45.5));
    var x = 1.0;
    for (i = 0; i < 20; i++) x *= 1.5;
    printh("grow " + x);
    var p = 0.0, v = 0.25;
    for (i = 0; i < 10; i++) { p += v; v -= 0.05; }
    printh("motion " + p + " " + v);
    x = 2.5;
    x++; x += 3; x -= 0.125; x *= 2; x /= 3; x %= 1.5;
    printh("compound " + x);
    var grid = new Int32Array(4), bytes = new Uint8Array(4);
    grid[1.9] = 7.75;
    grid[0] = -7.75;
    bytes[2] = 300.5;
    printh("typed " + grid + " " + bytes + " " + grid[1.2] + " " + [10, 20, 30][2.99]);
    printh("pow " + Math.pow(1.5, 3) + " " + Math.pow(2.0, -2) + " " + Math.pow(0.5, 10) +
           " " + Math.sqrt(2.0) + " " + Math.sqrt(2) + " " + Math.sqrt(10000.0));
    printh("minmax " + Math.min(3, 2.5, 7) + " " + Math.max(-1.5, -2) + " " +
           (Math.min(2, 2.0) == 2));
    printh("text " + 0.1 + " " + 0.00001 + " " + -0.00001 + " " + 99.99995 +
           " " + (1 / 3.0 * 3) + " " + typeof 0.5 + " " + parseInt(7.9));
    printh("truth " + (0.0 ? "y" : "n") + (0.5 ? "y" : "n") + (0.0 || "or") +
           (0.25 && "and"));
    if (2 == 2.0 && 1.5 > 1 && -0.5 < 0) printh("compare ok");
}

var tick = 0, pos = 0.0;
function update() {
    tick++;
    pos += 1.125;
    if (tick % 15 == 0) printh("tick " + tick + " " + pos + " " + (pos * pos));
}
