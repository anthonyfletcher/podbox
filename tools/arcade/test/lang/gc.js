// Allocation in a loop, so the collector runs many times with live data
// held across collections.
var keep = [];
var tick = 0;

function make(n) {
    var o = { id: n, name: "item" + n, list: [n, n + 1, n + 2] };
    o.extra = new Int32Array(8);
    o.extra[n % 8] = n;
    return o;
}

function init() {
    for (var i = 0; i < 200; i++) keep.push(make(i));
}

function update() {
    tick++;
    var junk = [];
    for (var i = 0; i < 300; i++) {
        junk.push("s" + i + "-" + tick);
        junk.push(make(i));
    }
    keep[tick % 200] = make(tick + 1000);
    var sum = 0;
    for (i = 0; i < keep.length; i++)
        sum = sum + keep[i].id + keep[i].list[2] + keep[i].extra[keep[i].id % 8];
    if (tick % 10 == 0) printh(tick + " " + sum + " " + keep[tick % 200].name +
                                " " + junk.length);
}
