// text: numbers, times and words laid out on the screen, in the current
// font and size.
//
//   text.pad(n, width)          n with noughts in front to width digits
//   text.clock(seconds, [whole])
//                               as m:ss.t, or m:ss when whole is true
//   text.lap(ms)                as m:ss.hh: a time board's milliseconds
//   text.width(s)               s's width in pixels
//   text.center(s, x, y, c)     s centred on x; on the screen when x is null
//   text.right(s, x, y, c)      s ending at x
//   text.outline(s, x, y, c, edge)
//                               s in c with a one-pixel edge round it
//   text.shadow(s, x, y, c, under)
//                               s in c over itself in under, a pixel down
//                               and right
//   text.box(s, x, y, w, c)     s wrapped at spaces to lines w pixels wide;
//                               returns the height
//
// Cutting a line short to fit, with "...", is textfit(): the engine's.

function pad(n, width) {
    var s = "" + n;
    while (s.length < width) s = "0" + s;
    return s;
}

function clock(seconds, whole) {
    var all = flr(seconds), m = flr(all / 60), s = all % 60;
    if (whole) return m + ":" + pad(s, 2);
    return m + ":" + pad(s, 2) + "." + flr((seconds - all) * 10);
}

function lap(ms) {
    var cs = flr(ms / 10), s = flr(cs / 100);
    return flr(s / 60) + ":" + pad(s % 60, 2) + "." + pad(cs % 100, 2);
}

function width(s) {
    return textw(s);
}

function center(s, x, y, c) {
    if (x == null) x = 80;
    print(s, x - flr(width(s) / 2), y, c);
}

function right(s, x, y, c) {
    print(s, x - width(s), y, c);
}

function shadow(s, x, y, c, under) {
    print(s, x + 1, y + 1, under);
    print(s, x, y, c);
}

function outline(s, x, y, c, edge) {
    for (var dy = -1; dy <= 1; dy++)
        for (var dx = -1; dx <= 1; dx++)
            if (dx != 0 || dy != 0) print(s, x + dx, y + dy, edge);
    print(s, x, y, c);
}

// the byte after the character starting at i
function after(s, i) {
    i++;
    while (i < s.length && (s.charCodeAt(i) & 0xc0) == 0x80) i++;
    return i;
}

// lines of whole words as wide as fit; a word wider than the box is cut
function box(s, x, y, w, c) {
    var line = texth(""), lines = 0, start = 0;
    while (start < s.length) {
        // skip the spaces a line would begin with
        while (start < s.length && s.charCodeAt(start) == 32) start++;
        if (start >= s.length) break;
        // the most characters that fit, at least one; strings are UTF-8
        // bytes, so a character is cut whole
        var end = after(s, start);
        while (end < s.length) {
            var next = after(s, end);
            if (textw(s.substring(start, next)) > w) break;
            end = next;
        }
        if (end < s.length && s.charCodeAt(end) != 32) {
            var back = end;
            while (back > start && s.charCodeAt(back) != 32) back--;
            if (back > start) end = back;
        }
        print(s.substring(start, end), x, y + lines * line, c);
        lines++;
        start = end;
    }
    return lines * line;
}
