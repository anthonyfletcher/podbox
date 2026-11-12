// The instructions the compiler fuses: a local against a small integer and
// jump (in if, while, for, &&, ||, !, ?:), a global's field, a global
// indexed by a local; and the inline index of arrays and typed arrays, in
// and out of range, with integers, fractions and strings.
var arr = [10, 20, "x", 1.5, null], i32 = new Int32Array(4), u8 = new Uint8Array(3);
var rec = { a: 1, b: "two", c: 3.25 }, word = "hello";
var keys = ["a", "b", "c", "d"];

function compare(v) {
    var out = "";
    if (v < 3) out = out + "lt"; else out = out + "ge";
    if (v <= 3) out = out + " le"; else out = out + " gt";
    if (v > -2) out = out + " gt-2"; else out = out + " le-2";
    if (v >= 127) out = out + " ge127"; else out = out + " lt127";
    if (!(v < -128)) out = out + " notlt-128";
    if (v < 3 && v > 0) out = out + " between";
    if (v < 0 || v > 5) out = out + " outside";
    out = out + (v >= 2 ? " ?ge2" : " ?lt2");
    return out;
}

function init() {
    var vals = [0, 1, 2, 3, 4, -1, -2, -3, 126, 127, 128, -128, -129, 2.5,
                -2.5, 3.0001, 2.9999, 127.5, -128.5];
    for (var i = 0; i < vals.length; i++)
        printh(vals[i] + ": " + compare(vals[i]));
    var n = 0;
    for (var j = 0; j < 10; j++) n += j;
    var k = 10;
    while (k > 0) k -= 3;
    var m = -5;
    do { m++; } while (m <= 2);
    printh("loops " + n + " " + k + " " + m);
    for (var f = 0; f < 2; f += 0.25) n += f;
    printh("fraction loop " + n);

    for (var a = -1; a < 7; a++)
        printh("arr " + a + " " + arr[a] + " i32 " + i32[a] + " u8 " + u8[a] +
               " word " + word[a]);
    for (var b = 0; b < 4; b++) {
        i32[b] = b * 1000 - 1500;
        u8[b] = b * 100 + 50;
        arr[b] = b * 2;
    }
    arr[5] = "appended";
    i32[1] = 2.75;
    i32[2] = -2.75;
    i32[9] = 7;
    for (var c = 0; c < 7; c++)
        printh("after " + c + " " + arr[c] + " " + i32[c] + " " + u8[c]);
    printh("length " + arr.length + " " + i32.length + " " + u8.length);

    for (var d = 0; d < keys.length; d++) {
        var key = keys[d];
        printh("rec " + key + " " + rec[key]);
    }
    printh("fields " + rec.a + " " + rec.b + " " + rec.c + " " + rec.zz);
    rec.a += 5;
    rec.zz = "new";
    printh("fields again " + rec.a + " " + rec.zz + " " + word.length);
    var h = 1.5;
    printh("fraction index " + arr[h] + " " + i32[h] + " " + word[h]);
}

function update() {}
function draw() {}
