// The track along the left of the band: the title and the artist, cropped
// to the room the score leaves. Where it does not fit it either steps on a
// character a beat, or -- with the Track name option on Still -- stays put,
// cut at the last whole character that fits with "..." after it.

const GAP = "      ";     // the blank before it comes round again
const MORE = "...";

var text = "";
var loop = "";              // text and the gap, twice: a window onto it
var wide = 0;
var step = 0;               // characters stepped, a beat each
var cut = "", cutRoom = -1; // the text cut to fit, and the room it was cut for

function still() {
    return opt.get(OPT.caption) == 1;
}

// What it says now; asked when the track changes
function update() {
    var s = playback.title(), a = playback.artist();
    if (a != null && a != "")
        s = s + " - " + a;
    s = s.toUpperCase();
    font(FONT.CAPS);
    if (s == text)
        return;
    text = s;
    loop = s + GAP + s + GAP;
    wide = textw(s);
    step = 0;
    cutRoom = -1;
}

// A beat gone by: the next step, where it is scrolling
function stepOn() {
    if (!still())
        step++;
}

// The text cut to fit room with MORE after it, worked out again only when
// the text or the room changes
function cutTo(room) {
    if (cutRoom == room)
        return cut;
    cut = textfit(text, room, MORE);
    cutRoom = room;
    return cut;
}

function draw(x, y, room) {
    if (text == "" || room < 24)
        return;
    clip(x, 0, x + room - 1, 29);
    if (wide <= room)
        print(text, x, y, field.INK);
    else if (still())
        print(cutTo(room), x, y, field.INK);
    else {
        var n = step % (text.length + GAP.length);
        print(loop.substring(n), x, y, field.INK);
    }
    clip();
}
