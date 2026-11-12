// The end of a run, and the record: the score, with the crown when it beat
// the best; what it came to, beside it; and under them the tracks it was
// played over, which the wheel scrolls. The record is a run that ended some
// other evening, so one screen shows both.

const LIST_Y = 44, LIST_ROWS = 7, ROW_H = 8;

var shown = null;           // the run on screen: score, beats, secs, bpm10, tracks
var beaten = false, rank = 0, isRecord = false;
var first = 0;              // the top track shown

// The record so far, or null
function record() {
    return store.read("record");
}

// A run that has ended kept: on the board, and as the record when it beat
// it -- or when there is none
function keep(run) {
    var was = record();
    beaten = was != null && run.score > was.score;
    if (was == null || run.score > was.score)
        store.write("record", run);
    rank = score.submit(BOARD.points, run.score);
}

// ...and shown, when it ended with the music
function ended(run) {
    keep(run);
    isRecord = false;
    show(run);
}

// The record, looked at from the pause menu; false when there is none
function showRecord() {
    var r = record();
    if (r == null)
        return false;
    beaten = false;
    rank = 0;
    isRecord = true;
    show(r);
    return true;
}

function show(run) {
    shown = run;
    first = 0;
}

// True while it stays up
function update() {
    var n = shown.tracks.length, w = wheel();
    if (w != 0)
        first = mid(0, first + (w > 0 ? 1 : -1), max(0, n - LIST_ROWS));
    return !btnp(4);
}

// A track: its title, and its genre on the right in a quieter shade. A
// record kept before genres were is a list of titles alone.
function trackRow(e, y) {
    var title = typeof e == "string" ? e : e.t, genre = typeof e == "string" ? "" : e.g;
    var gx = 149 - textw(genre.toUpperCase());
    if (genre != "")
        print(genre.toUpperCase(), gx, y, field.INK3);
    clip(8, y, (genre != "" ? gx - 6 : 149) - 1, y + ROW_H - 1);
    print(title.toUpperCase(), 8, y, field.INK);
    clip(8, LIST_Y, 149, LIST_Y + LIST_ROWS * ROW_H - 1);
}

// What the run came to, in a column on the right of the score
function row(label, value, y) {
    print(label, 88, y, field.INK3);
    print(value, 151 - textw(value), y, field.INK);
}

function draw() {
    cls(field.PAPER);
    camera();
    font(FONT.CAPS);
    print(isRecord ? "THE BEST RUN" : beaten ? "A NEW BEST" : "THE RUN", 8, 6, field.INK3);
    // the crown at the left, the score after it on the crown's middle: the
    // crown is ch + 1 rows, the digits field.CAP_H
    var s = field.pad7(shown.score), ch = texth("0"), x = 8;
    if (beaten || isRecord) {
        field.crown(x, 16, ch);
        x += ch + 3;
    }
    print(s, x, 16 + (ch + 1 - field.CAP_H) / 2, field.INK);
    var secs = shown.secs;
    row("BEATS", "" + shown.beats, 6);
    row("TIME", flr(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + secs % 60, 14);
    row("TEMPO", shown.bpm10 > 0 ? flr(shown.bpm10 / 10) + " BPM" : "-", 22);
    if (rank > 0)
        row("RANK", "" + rank, 30);

    // the tracks, under a rule, the width of the screen
    var n = shown.tracks.length;
    print(n + (n == 1 ? " TRACK" : " TRACKS"), 8, LIST_Y - 12, field.INK3);
    line(8, LIST_Y - 4, 151, LIST_Y - 4, field.INK4);
    clip(8, LIST_Y, 149, LIST_Y + LIST_ROWS * ROW_H - 1);
    for (var i = 0; i < LIST_ROWS && first + i < n; i++)
        trackRow(shown.tracks[first + i], LIST_Y + i * ROW_H);
    clip();
    if (n > LIST_ROWS) {
        var h = LIST_ROWS * ROW_H, bar = max(3, h * LIST_ROWS / n);
        var y = LIST_Y + (h - bar) * first / max(1, n - LIST_ROWS);
        rectfill(152, y, 153, y + bar - 1, field.INK3);
    }
    print(isRecord ? "SELECT: BACK" : "SELECT: ANOTHER RUN", 8, 109, field.INK3);
}
