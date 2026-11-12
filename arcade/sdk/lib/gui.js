// gui: the pieces the ui dialogs are made of, for a cart's own screens --
// drawn the same way, in the current font and size.
//
//   gui.style(box, text, highlight)
//                       the three colours; dark blue, white and yellow
//                       until called, as the dialogs'
//   gui.box(x0, y0, x1, y1)       a panel: filled, with an edge
//   gui.lines(s, w)     s broken into lines w pixels wide, an array: at
//                       each newline, at the last space that fits, or
//                       inside a word too long for a line
//   gui.button(label, x, y, lit)  a button at x, y, highlighted when lit;
//                       returns its width
//   gui.buttons(labels, chosen, lit, x, y, w)
//                       buttons spread along a row centred in x to x+w,
//                       the one at chosen highlighted when lit
//   gui.bar(x, y, h, first, shown, total)
//                       a scroll bar down x, from y for h pixels: shown
//                       of total from first; nothing when all are shown
//
//   l = gui.list(items, rows)     a list showing rows of items at a time
//   gui.list_update(l)  once a tick: the wheel and PREV and NEXT move the
//                       choice, kept in sight; the index when SELECT is
//                       pressed, else -1
//   gui.list_draw(l, x, y, w)     the rows from x, y, w wide, with the
//                       scroll bar at the right
//   l.at                the index chosen now
//
//   f = gui.field(most, start, chars)
//                       a line of at most most bytes, starting as start
//                       (or empty), typed from chars (or space, A-Z, 0-9
//                       and -_.'&)
//   gui.field_update(f) once a tick: the wheel puts a character in at the
//                       caret and turns it, PREV and NEXT keep it and move,
//                       held half a second they delete; 1 when SELECT is
//                       pressed, else 0
//   gui.field_draw(f, x, y, w, lit)
//                       the line in its frame, w wide and texth("") + 4
//                       high, with the caret when lit is not 0
//   gui.field_text(f)   the line, without spaces at its ends
//
// A list and a field speak what is chosen when the owner has menus spoken.
// Text a cart must not drop goes through ui.text instead, which also
// follows the owner's language in FONT.WORLD.

var ink_box = 1, ink_text = 7, ink_hi = 10;

function style(box, text, highlight) {
    ink_box = box;
    ink_text = text;
    ink_hi = highlight;
}

function box(x0, y0, x1, y1) {
    rectfill(x0, y0, x1, y1, ink_box);
    rect(x0, y0, x1, y1, ink_text);
}

// the byte after the character starting at i
function after(s, i) {
    i++;
    while (i < s.length && (s.charCodeAt(i) & 0xc0) == 0x80) i++;
    return i;
}

function lines(s, w) {
    var out = [], i = 0, n = s.length;
    while (i < n) {
        var end = i, space = -1;
        while (end < n && s.charCodeAt(end) != 10) {
            var next = after(s, end);
            if (end > i && textw(s.substring(i, next)) > w) break;
            if (s.charCodeAt(end) == 32) space = end;
            end = next;
        }
        var stop = end, from = end;
        if (end < n && s.charCodeAt(end) != 10 && space > i) {
            stop = space;
            from = space + 1;
        } else if (end < n && s.charCodeAt(end) == 10) from = end + 1;
        out.push(s.substring(i, stop));
        i = from;
    }
    return out;
}

function button(label, x, y, lit) {
    var w = textw(label);
    if (lit) rectfill(x - 3, y, x + w + 1, y + texth("") + 1, ink_hi);
    print(label, x, y + 1, lit ? ink_box : ink_text);
    return w;
}

function buttons(labels, chosen, lit, x, y, w) {
    var total = 0, i;
    for (i = 0; i < labels.length; i++) total += textw(labels[i]);
    var bx = x + flr((w - total - 12 * (labels.length - 1)) / 2);
    for (i = 0; i < labels.length; i++)
        bx += button(labels[i], bx, y, lit && i == chosen) + 12;
}

function bar(x, y, h, first, shown, total) {
    if (total <= shown || h < 4) return;
    var len = flr(h * shown / total), at = y + flr(h * first / total);
    if (len < 2) len = 2;
    rectfill(x, at, x, at + len - 1, ink_text);
}

// the wheel's whole steps of per clicks, the rest kept in o; a fast spin
// counts double
function steps(o, per) {
    var w = wheel();
    o.acc += w;
    var s = o.acc >= 0 ? flr(o.acc / per) : -flr(-o.acc / per);
    o.acc -= s * per;
    if (w >= 3 * per || w <= -3 * per) s *= 2;
    return s;
}

function say(s) {
    if (speak.on() && s.length > 0) speak.say(s);
}

function list(items, rows) {
    return { items: items, at: 0, top: 0, rows: min(rows, items.length) };
}

function list_update(l) {
    var before = l.at, n = l.items.length;
    l.at += wheelsteps();
    if (btnp(0)) l.at--;
    if (btnp(1)) l.at++;
    l.at = mid(0, l.at, n - 1);
    if (l.at < l.top) l.top = l.at;
    if (l.at >= l.top + l.rows) l.top = l.at - l.rows + 1;
    if (l.at != before) say(l.items[l.at]);
    return btnp(4) ? l.at : -1;
}

function list_draw(l, x, y, w) {
    var line = texth(""), row = line + 2;
    clip(x, y, x + w - 5, y + l.rows * row - 1);
    for (var r = 0; r < l.rows; r++) {
        var i = l.top + r, on = i == l.at, ry = y + r * row;
        if (on) rectfill(x, ry, x + w - 5, ry + line + 1, ink_hi);
        print(l.items[i], x + 2, ry + 1, on ? ink_box : ink_text);
    }
    clip();
    bar(x + w - 3, y, l.rows * row, l.top, l.rows, l.items.length);
}

function field(most, start, chars) {
    var f = { line: [], caret: 0, editing: 0, most: most, cycle: [],
              scroll: 0, acc: 0, held_prev: 0, held_next: 0 };
    var i = 0, s = start == null ? "" : start;
    while (i < s.length) { var j = after(s, i); f.line.push(s.substring(i, j)); i = j; }
    f.caret = f.line.length;
    s = chars == null ? " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.'&" : chars;
    i = 0;
    while (i < s.length) { j = after(s, i); f.cycle.push(s.substring(i, j)); i = j; }
    return f;
}

function field_bytes(f) {
    var n = 0;
    for (var i = 0; i < f.line.length; i++) n += f.line[i].length;
    return n;
}

// the wheel turned dir steps at the caret: a character put in first when
// none is being chosen, then turned through the cycle, past any that
// would make the line too long
function turn(f, dir) {
    if (!f.editing) {
        if (field_bytes(f) + 1 > f.most) return;
        f.line.push(" ");
        for (var i = f.line.length - 1; i > f.caret; i--) f.line[i] = f.line[i - 1];
        f.line[f.caret++] = " ";
        f.editing = 1;
    }
    var c = f.line[f.caret - 1], n = f.cycle.length, idx = 0;
    while (idx < n && f.cycle[idx] != c) idx++;
    if (idx == n) idx = 0;
    var room = f.most - field_bytes(f) + c.length;
    for (var tries = 0; tries < n; tries++) {
        idx = ((idx + dir) % n + n) % n;
        if (f.cycle[idx].length <= room) break;
    }
    f.line[f.caret - 1] = f.cycle[idx];
}

function remove(f, at) {
    if (at < 0 || at >= f.line.length) return;
    f.line.splice(at, 1);
    if (f.caret > at) f.caret--;
    f.editing = 0;
}

// a button held, in ticks: 1 for a tap as it is let go, 2 each time a
// hold of half a second, then each sixth, repeats
function repeat(count, down) {
    var first = flr(rate() / 2), every = max(1, flr(rate() / 6));
    if (down) {
        count++;
        return count >= first && (count - first) % every == 0 ? 2 : 0;
    }
    return count > 0 && count < first ? 1 : 0;
}

function field_update(f) {
    var before = f.editing ? f.line[f.caret - 1] : null;
    for (var s = steps(f, 6); s != 0; s += s > 0 ? -1 : 1) turn(f, s > 0 ? 1 : -1);
    var left = repeat(f.held_prev, btn(0)), right = repeat(f.held_next, btn(1));
    f.held_prev = btn(0) ? f.held_prev + 1 : 0;
    f.held_next = btn(1) ? f.held_next + 1 : 0;
    if (left == 1 && f.caret > 0) { f.caret--; f.editing = 0; }
    else if (left == 2) remove(f, f.caret - 1);
    if (right == 1 && f.caret < f.line.length) { f.caret++; f.editing = 0; }
    else if (right == 1) f.editing = 0;
    else if (right == 2) remove(f, f.caret);
    if (f.editing && f.line[f.caret - 1] != before) say(f.line[f.caret - 1]);
    if (btnp(4)) { f.editing = 0; return 1; }
    return 0;
}

function field_draw(f, x, y, w, lit) {
    var line = texth(""), x1 = x + w - 1, text = "", mark = 0, upto = 0;
    rect(x, y, x1, y + line + 3, ink_text);
    for (var i = 0; i < f.line.length; i++) {
        if (i == f.caret - 1) mark = text.length;
        text += f.line[i];
        if (i == f.caret - 1) upto = text.length;
    }
    var cx = textw(text.substring(0, upto)), room = x1 - x - 4;
    if (cx - f.scroll > room) f.scroll = cx - room;
    if (cx < f.scroll) f.scroll = cx;
    var tx = x + 2 - f.scroll;
    clip(x + 1, y + 1, x1 - 1, y + line + 2);
    print(text, tx, y + 2, ink_text);
    if (lit != 0) {
        if (f.editing) {
            var mx = textw(text.substring(0, mark));
            rectfill(tx + mx - 1, y + 1, tx + cx - 1, y + line + 2, ink_hi);
            print(text.substring(mark, upto), tx + mx, y + 2, ink_box);
        } else rectfill(tx + cx - 1, y + 2, tx + cx - 1, y + line + 1, ink_hi);
    }
    clip();
}

function field_text(f) {
    var a = 0, b = f.line.length, s = "";
    while (a < b && f.line[a] == " ") a++;
    while (b > a && f.line[b - 1] == " ") b--;
    for (var i = a; i < b; i++) s += f.line[i];
    return s;
}
