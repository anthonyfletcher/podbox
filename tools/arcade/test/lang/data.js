// Strings, arrays, typed arrays and records.
var bytes = new Uint8Array(16);
var wide = new Int32Array(4);
var people = [{ name: "b", age: 30 }, { name: "a", age: 30 }, { name: "c", age: 20 }];

function init() {
    var s = "Hello, World";
    printh(s.length + " " + s.charCodeAt(0) + " " + s.charCodeAt(99) + " " +
           s.indexOf("o") + " " + s.indexOf("zz") + " " + s.indexOf(""));
    printh(s.substring(7) + "|" + s.substring(5, 0) + "|" + s.substring(-3, 2) +
           "|" + s.slice(-5) + "|" + s.slice(2, -3) + "|" + s[4] + "|" + s[99]);
    printh(s.toUpperCase() + s.toLowerCase() + String.fromCharCode(65) + "!");
    printh(("b" < "a") + " " + ("abc" < "abd") + " " + ("ab" < "abc") + " " +
           ("x" == "x") + " " + ("1" == 1) + " " + (s == "Hello, World"));
    printh(parseInt("42") + " " + parseInt("  -17xyz") + " " + parseInt("0x1F") +
           " " + parseInt("ff", 16) + " " + parseInt("z") + " " + parseInt(9));
    printh(1 + 2 + "3" + 4 + 5);

    var a = [5, 3, 8];
    printh(a.push(1) + " " + a + " " + a.pop() + " " + a.shift() + " " + a);
    printh(a.unshift(9) + " " + a + " " + a.indexOf(8) + " " + a.indexOf(7));
    a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    printh(a.splice(2, 3) + " | " + a + " | " + a.splice(-2) + " | " + a);
    printh(a.slice(1, 3) + " | " + a.slice(-2) + " | " + a.reverse() + " | " +
           a.join("") + " | " + a.join());
    a[a.length] = 99;
    a[0] = null;
    printh(a + " " + a.length + " " + a[50] + " " + (a[50] == null));
    var nested = [1, [2, [3, null]], "x"];
    printh("nested " + nested + " " + nested[1][1][0]);

    var big = [];
    for (var i = 0; i < 100; i++) big.push((i * 37) % 101);
    big.sort(function (x, y) { return x - y; });
    printh("sorted " + big.slice(0, 10) + " .. " + big.slice(95));
    people.sort(function (p, q) { return p.age - q.age; });
    printh("stable " + people[0].name + people[1].name + people[2].name);

    for (i = 0; i < bytes.length; i++) bytes[i] = i * 40;
    printh("u8 " + bytes + " " + bytes[99] + " " + bytes.indexOf(160));
    bytes[99] = 5;
    wide[0] = 0x7fffffff; wide[1] = -1; wide[3] += 7; wide[2]--;
    printh("i32 " + wide + " " + wide.length + " " + wide.reverse());

    var o = { x: 1, y: "two", z: [3] };
    o.w = 4;
    o["v"] = 5;
    var key = "x";
    o[key] += 10;
    printh("rec " + o.x + " " + o.y + " " + o.z[0] + " " + o.w + " " + o.v +
           " " + o.missing + " " + o["miss"] + " " + o[key] + " " + o);
    var counts = {};
    var words = ["a", "b", "a", "c", "a", "b"];
    for (i = 0; i < words.length; i++) {
        var w = words[i];
        counts[w] = (counts[w] || 0) + 1;
    }
    printh("dict " + counts.a + counts.b + counts.c);
    printh(typeof o + typeof s + typeof 1 + typeof null + typeof init + typeof bytes);
}

function update() {}
