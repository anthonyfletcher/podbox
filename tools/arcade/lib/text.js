// text: numbers, times and words laid out on the screen. Widths are the
// small font's, four pixels a character; a cart drawing in FONT.BIG uses
// twice them.
//
//   text.pad(n, width)          n with noughts in front to width digits
//   text.clock(seconds)         as m:ss.t
//   text.width(s)               s's width in pixels
//   text.centre(s, x, y, c)     s centred on x
//   text.right(s, x, y, c)      s ending at x
//   text.outline(s, x, y, c, edge)
//                               s in c with a one-pixel edge round it
//   text.box(s, x, y, w, c)     s wrapped at spaces to lines w pixels wide,
//                               a line every six pixels; returns the height

const CHAR = 4, LINE = 6;

function pad(n, width) {
    var s = "" + n;
    while (s.length < width) s = "0" + s;
    return s;
}

function clock(seconds) {
    var whole = flr(seconds), m = flr(whole / 60), s = whole % 60;
    var tenth = flr((seconds - whole) * 10);
    return m + ":" + pad(s, 2) + "." + tenth;
}

function width(s) {
    return s.length * CHAR;
}

function centre(s, x, y, c) {
    print(s, x - flr(width(s) / 2), y, c);
}

function right(s, x, y, c) {
    print(s, x - width(s), y, c);
}

function outline(s, x, y, c, edge) {
    for (var dy = -1; dy <= 1; dy++)
        for (var dx = -1; dx <= 1; dx++)
            if (dx != 0 || dy != 0) print(s, x + dx, y + dy, edge);
    print(s, x, y, c);
}

// lines of whole words as wide as fit; a word wider than the box is cut
function box(s, x, y, w, c) {
    var per = max(1, flr(w / CHAR)), lines = 0, start = 0;
    while (start < s.length) {
        // skip the spaces a line would begin with
        while (start < s.length && s.charCodeAt(start) == 32) start++;
        if (start >= s.length) break;
        var end = min(start + per, s.length);
        if (end < s.length && s.charCodeAt(end) != 32) {
            var back = end;
            while (back > start && s.charCodeAt(back) != 32) back--;
            if (back > start) end = back;
        }
        print(s.substring(start, end), x, y + lines * LINE, c);
        lines++;
        start = end;
    }
    return lines * LINE;
}
