// Integer arithmetic at its edges: wrapping, truncation, shifts.
const BIG = 0x7fffffff;
var vals = [0, 1, -1, 2, -2, 7, -7, 100, -100, 65536, -65536, BIG, -BIG - 1,
            0x55555555, -0x12345678, 31, 32, 33];

function init() {
    var i, j, a, b;
    for (i = 0; i < vals.length; i++) {
        a = vals[i];
        printh("neg " + a + " " + -a + " " + ~a + " " + !a);
        for (j = 0; j < vals.length; j++) {
            b = vals[j];
            var line = a + " " + b + ": " + (a + b) + " " + (a - b) + " " +
                (a * b) + " " + (a & b) + " " + (a | b) + " " + (a ^ b) + " " +
                (a << b) + " " + (a >> b) + " " + (a >>> b) + " " +
                (a < b) + (a <= b) + (a > b) + (a >= b) + (a == b) + (a != b);
            if (b != 0) line = line + " " + (a / b) + " " + (a % b);
            printh(line);
        }
    }
    var x = 5;
    x += 3; x -= 10; x *= -3; x /= 4; x %= 4; x <<= 3; x >>= 1; x >>>= 1;
    x &= 0xff; x |= 0x100; x ^= 0x0f;
    printh("compound " + x);
    var y = BIG;
    y++;
    printh("inc wraps " + y);
    y--;
    printh("dec wraps " + y);
    printh("pow " + Math.pow(3, 5) + " " + Math.pow(2, 31) + " " + Math.pow(2, -1) +
           " " + Math.pow(-1, -3) + " " + Math.pow(7, 0));
    printh("math " + Math.abs(-5) + " " + Math.min(4, -2, 9) + " " +
           Math.max(4, -2, 9) + " " + Math.sign(-9) + Math.sign(0) + Math.sign(3) +
           " " + Math.sqrt(99) + " " + Math.floor(-7 / 2));
}

function update() {}
