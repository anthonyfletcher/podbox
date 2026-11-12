// Array and string methods taking several values or a place to start:
// push and unshift of many, splice putting items in (fractions kept),
// indexOf from a place, and lastIndexOf, on arrays, typed arrays and
// strings.
var u = new Uint8Array(6);

function init() {
    var a = [1];
    printh(a.push(2, 3, 4) + " " + a);
    a = [3];
    printh(a.unshift(1, 2) + " " + a);

    a = [1, 2, 3, 4];
    printh(a.splice(1, 2, 9, 8, 7) + " | " + a);
    a = [1, 4];
    a.splice(1, 0, 2, 3);
    printh(a + " " + a.length);
    a = [1, 2, 3, 4, 5];
    printh(a.splice(1, 3, 9) + " | " + a + " " + a.length);
    a = [1, 2, 3];
    printh(a.splice(-1, 1, "x", "y") + " | " + a);
    // what splice inserts is kept as it is, a fraction too
    a = [1, 4];
    a.splice(1.0, 0, 2.5, 0.25);
    printh(a);

    a = [5, 6, 5, 6];
    printh(a.indexOf(5, 1) + " " + a.indexOf(6, -1) + " " + a.indexOf(5, 9) + " " + a.indexOf(5, -99));
    printh(a.lastIndexOf(5) + " " + a.lastIndexOf(6, 2) + " " + a.lastIndexOf(6, -3) + " " +
           a.lastIndexOf(7) + " " + a.lastIndexOf(5, 99) + " " + a.lastIndexOf(5, -99));

    var s = "abcabc";
    printh(s.indexOf("b", 2) + " " + s.lastIndexOf("b") + " " + s.lastIndexOf("b", 3) + " " +
           s.lastIndexOf("c", 0) + " " + s.lastIndexOf("") + " " + s.lastIndexOf("abc", 1));

    u[0] = 3;
    u[2] = 3;
    printh(u.indexOf(3, 1) + " " + u.lastIndexOf(3) + " " + u.lastIndexOf(0) + " " + u.lastIndexOf(3, 1));
}

function update() {}
