// menu: an in-game menu driven by the wheel and SELECT, drawn in the
// cart's own colours.
//
//   menu.open(items)        items, an array of strings, with the first
//                           chosen
//   menu.update()           once a tick while open: the wheel moves the
//                           choice a line a detent (sixteen to a turn),
//                           PREV and NEXT a line a press; returns the line
//                           chosen when SELECT is pressed, else -1
//   menu.draw(x, y, c, bar) the lines from x, y, six pixels apart, the
//                           chosen one on a bar of colour bar
//   menu.choice()           the line chosen now
//
// It moves with dial.steps(), so a cart using menu needs dial too.

var lines = [];
var at = 0;

function open(items) {
    lines = items;
    at = 0;
}

function update() {
    if (lines.length == 0) return -1;
    var n = lines.length;
    at = (at + dial.steps(16) + n * 16) % n;
    if (btnp(0)) at = (at + n - 1) % n;
    if (btnp(1)) at = (at + 1) % n;
    if (btnp(4)) return at;
    return -1;
}

function draw(x, y, c, bar) {
    var wide = 0;
    for (var i = 0; i < lines.length; i++) wide = max(wide, lines[i].length * 4);
    for (i = 0; i < lines.length; i++) {
        if (i == at) rectfill(x - 2, y + i * 6 - 1, x + wide, y + i * 6 + 5, bar);
        print(lines[i], x, y + i * 6, c);
    }
}

function choice() {
    return at;
}
