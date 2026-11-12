// Music Quiz: a track plays from partway in, five answers are listed --
// titles, artists, albums or years, as the round asks -- and the points for
// the round drain away until one is picked. Ten rounds, each harder than
// the last (deal.js).
//
// The clock that drains the points starts when the clip is heard, not when
// the round starts: spinning the disk up and seeking into a track is not
// the player's time to lose. Nothing a clip plays counts as a listen, and
// the owner's queue is set aside for the game and put back after it.
//
// A pause -- Play, the pause menu or the hold switch -- loses the round.
//
// With speech on the quiz can be played by ear: the question and the five
// answers are read before the clip starts -- the clip waits for the
// reading, or for the wheel to cut it short -- each answer is said as it is
// reached, and the verdict and the final score are spoken.
//
// Parts: colours; the states; one round; the results; drawing.
// The title screen is title.js.

const ROUND_POINTS = 100;
const HZ = 25;                      // the rate, in cart.json
const ROUND_TICKS = 12 * HZ;        // what a round takes to drain
const REVEAL_TICKS = HZ * 3 / 2;    // the answer on screen
const START_TIMEOUT = 6 * HZ;       // a clip not heard by now starts anyway

const S_TITLE = 0, S_DEAL = 1, S_FAILED = 2, S_ROUND = 3, S_RESULT = 4;
const M_LISTENING = 0, M_PLAYING = 1, M_ANSWER = 2;

// ---- colours --------------------------------------------------------------

// From 18 on: the palette's 16 and 17 are the title's blues
const C_BACK = 18, C_TEXT = 19, C_ROW_TEXT = 20, C_DIM = 21, C_FADED = 22;
const C_ROW = 23, C_ROW_FADED = 24, C_SEL = 25, C_SEL_FADED = 26;
const C_SEL_FADED_TEXT = 27, C_RIGHT = 28, C_RIGHT_TEXT = 29, C_WRONG = 30;
const C_WRONG_MARK = 31, C_WRONG_TEXT = 32, C_MISS = 33, C_TROUGH = 34;
const C_BAR = 35, C_BAR_FADED = 36, C_WHITE = 37;
// The title screen's, the logo's yellow and navy from the first 16
const C_LOGO = 10, C_LOGO_INK = 1;

var colours = [0x111217, 0xe8eaf0, 0xd9dce4, 0x8a8f9c, 0x6b7080, 0x23252e,
               0x191a20, 0x3d6fd6, 0x26375c, 0x9aa6c4, 0x2e9d5a, 0x4cc27f,
               0x9e3a31, 0xc4493e, 0xe8796f, 0x4a2522, 0x2c2e37, 0xf0aa28,
               0x6e5a33, 0xffffff];

// What each kind of round asks
var asks = ["Name the Track", "Name the Artist", "Name the Album",
            "Name the Year"];

// ---- the states -----------------------------------------------------------

var state = S_TITLE;
var dealt = 0.0;        // how far the deal has come, 0 to 1
var deal_ticks = 0;

// The game: the round being played, the score before it, and each
// finished round's points and verdict
var rd = 0, points_total = 0;
var earned = [], was_right = [];

// The round: what an answer is worth now, the row chosen, and the row
// picked once it is over (-1 for time up, -2 for a pause)
var mode = M_LISTENING, points = 0, sel = 0, picked = -1;
var paused = false, reading = false, clip_on = false, started = false;
var waited = 0, spent = 0, revealed = 0;
var fits = [], fits_marked = [];

// The results
var record = false, best = 0;

function init() {
    for (var i = 0; i < colours.length; i++) setpal(C_BACK + i, colours[i]);
    to_title();
}

function to_title() {
    state = S_TITLE;
    title.start();
    if (speak.on()) speak.say("Music Quiz.", "Select to play");
}

// The owner's music stops for a quiz, if they say so
function from_title() {
    if (playback.playing()) {
        if (!ui.confirm("Stop the music for a quiz?", "Music Quiz")) return;
        playback.stop();
    }
    new_game();
}

function new_game() {
    deal.start(opt.get(OPT.titles), opt.get(OPT.artists),
               opt.get(OPT.albums), opt.get(OPT.years));
    state = S_DEAL;
    dealt = 0;
    deal_ticks = 0;
}

function update() {
    // asked every tick, so a pause is only ever this tick's
    var was_paused = resumed();
    if (state == S_TITLE) {
        title.update();
        if (btnp(4)) from_title();
    } else if (state == S_DEAL) {
        // a frame of the wait drawn before the first walk of the library
        if (deal_ticks++ > 0) dealt = deal.step();
        if (deal.failed) {
            state = S_FAILED;
            if (speak.on()) speak.say("Not enough music for a quiz");
        } else if (deal.done) {
            rd = 0;
            points_total = 0;
            earned = [];
            was_right = [];
            state = S_ROUND;
            begin_round();
        }
    } else if (state == S_FAILED) {
        if (btnp(4)) to_title();
    } else if (state == S_ROUND) {
        round_update(was_paused);
    } else if (btnp(4)) {
        new_game();
    }
}

// ---- one round ------------------------------------------------------------

// Where a clip starts: far enough in to be past an introduction, and inside
// the stretch the Sound Index measures, so a round whose wrong titles were
// matched by sound sounds like what they were matched against
function clip_start(length) {
    return mid(5000, flr(length * 15 / 100), 30000);
}

function start_clip() {
    var r = deal.rounds[rd];
    playback.play(r.id, clip_start(r.len));
    clip_on = true;
    waited = 0;
}

function begin_round() {
    var r = deal.rounds[rd];
    playback.stop();
    mode = M_LISTENING;
    points = ROUND_POINTS;
    sel = 0;
    picked = -1;
    paused = false;
    clip_on = false;
    started = false;
    spent = 0;
    font(FONT.WORLD);
    for (var i = 0; i < deal.CHOICES; i++) {
        fits[i] = textfit(r.choice[i], 144);
        fits_marked[i] = textfit(r.choice[i], 134);
    }
    font(FONT.CAPS);

    // the clip waits while the answers are read
    reading = false;
    if (speak.on()) {
        speak.say(asks[r.kind]);
        for (var k = 0; k < deal.CHOICES; k++)
            speak.queue(k + 1, r.choice[k] + ".");
        reading = speak.busy();
    }
    if (!reading) start_clip();
}

// Play: the round stops, and is lost when Play goes on
function pause_round() {
    paused = true;
    reading = false;
    speak.stop();
    playback.stop();
    if (speak.on()) speak.say("Paused. This round is lost.", "Play to go on");
}

function round_update(was_paused) {
    if (mode == M_ANSWER) {
        // the verdict heard before the next question
        if (++revealed >= REVEAL_TICKS && !speak.busy()) {
            if (++rd < deal.ROUNDS) begin_round();
            else end_game();
        }
        return;
    }
    if (btnp(3)) {
        if (paused) answer(-2);
        else pause_round();
        return;
    }
    if (paused) return;
    if (was_paused) {
        answer(-2);
        return;
    }

    var steps = wheelsteps(), was = sel;
    sel = mid(0, sel + steps, deal.CHOICES - 1);
    if (sel != was && speak.on())
        speak.say(sel + 1, deal.rounds[rd].choice[sel] + ".");

    // the reading is over, or the wheel cut it short: the clip
    if (reading && (!speak.busy() || steps != 0)) {
        reading = false;
        start_clip();
    }
    if (reading) return;

    // not before the clip: the clock has not started
    if (btnp(4)) {
        answer(sel);
        return;
    }
    if (!started && (playback.started() || ++waited > START_TIMEOUT)) {
        started = true;
        mode = M_PLAYING;
    }
    if (started) {
        spent++;
        points = max(0, ROUND_POINTS - flr(spent * ROUND_POINTS / ROUND_TICKS));
        if (points == 0) answer(-1);
    }
}

function answered_right() {
    return mode == M_ANSWER && picked == deal.rounds[rd].right;
}

function answer(row) {
    var r = deal.rounds[rd];
    playback.stop();
    mode = M_ANSWER;
    picked = row;
    paused = false;
    revealed = 0;
    was_right[rd] = answered_right();
    earned[rd] = was_right[rd] ? points : 0;
    points_total += earned[rd];
    if (!speak.on()) return;
    if (was_right[rd]) speak.say("Right", points);
    else speak.say(row == -2 ? "Paused" : row < 0 ? "Time's up" :
                   "Not that one", r.choice[r.right]);
}

// ---- the results ----------------------------------------------------------

function end_game() {
    var before = score.best(BOARD.points);
    best = before == null ? 0 : before;
    record = points_total > best;
    if (record) best = points_total;
    score.submit(BOARD.points, points_total);
    state = S_RESULT;
    if (!speak.on()) return;
    speak.say(points_total, "of " + deal.ROUNDS * ROUND_POINTS + ".");
    if (record) speak.queue("New best");
    else speak.queue("Best " + best);
    speak.queue("Select", "play again", "Menu", "leave");
}

// ---- drawing --------------------------------------------------------------

function centred(s, y, c) {
    print(s, flr((160 - textw(s)) / 2), y, c);
}

function draw() {
    cls(state == S_TITLE ? C_LOGO : C_BACK);
    if (state == S_TITLE) draw_title();
    else if (state == S_DEAL) draw_deal();
    else if (state == S_FAILED) draw_failed();
    else if (state == S_ROUND) draw_round();
    else draw_result();
}

function draw_title() {
    title.draw();
    if (frame() % 40 < 28) centred("SELECT TO PLAY", 100, C_LOGO_INK);
    var top = score.best(BOARD.points);
    if (top != null) centred("BEST " + top, 106, C_LOGO_INK);
}

function draw_deal() {
    centred("MUSIC QUIZ", 44, C_DIM);
    centred("CHOOSING THE TRACKS...", 56, C_TEXT);
    rectfill(30, 68, 129, 70, C_TROUGH);
    if (dealt > 0) rectfill(30, 68, 30 + flr(99 * dealt), 70, C_BAR);
}

function draw_failed() {
    centred("NOT ENOUGH MUSIC", 44, C_TEXT);
    centred("FOR A QUIZ", 52, C_TEXT);
    centred("SELECT TO GO BACK", 72, C_DIM);
}

// What this round asks, then the ten rounds as squares, then the score.
// The squares sit against the widest score there can be, so they stay put
// as the score grows a digit.
function draw_head() {
    var r = deal.rounds[rd];
    var total = points_total;
    var pips_x = 156 - 16 - 4 - (deal.ROUNDS * 6 - 2);
    for (var i = 0; i < deal.ROUNDS; i++) {
        var c = C_TROUGH;
        if (i < rd) c = was_right[i] ? C_RIGHT : C_WRONG_MARK;
        else if (i == rd && mode == M_ANSWER)
            c = answered_right() ? C_RIGHT : C_WRONG_MARK;
        else if (i == rd) c = C_TEXT;
        rectfill(pips_x + i * 6, 4, pips_x + i * 6 + 3, 7, c);
    }
    print("" + total, 156 - textw("" + total), 3, C_TEXT);
    print(asks[r.kind], 4, 3, C_TEXT);
}

// Listening, the draining bar, or the verdict -- and what an answer is worth
function draw_timer() {
    var c = C_BAR, bar_w = 132;
    if (mode == M_LISTENING) {
        // a quaver
        circfill(6, 17, 1, C_DIM);
        line(7, 12, 7, 17, C_DIM);
        line(8, 12, 9, 13, C_DIM);
        print("LISTENING...", 12, 13, C_DIM);
        c = C_DIM;
    } else if (mode == M_PLAYING) {
        rectfill(4, 14, 4 + bar_w - 1, 16, C_TROUGH);
        if (points > 0)
            rectfill(4, 14, 4 + flr(bar_w * points / ROUND_POINTS) - 1, 16,
                     paused ? C_BAR_FADED : C_BAR);
        if (paused) c = C_DIM;
    } else {
        var right = answered_right();
        print(right ? "RIGHT" : picked == -2 ? "PAUSED" :
              picked < 0 ? "TIME'S UP" : "NOT THAT ONE",
              4, 13, right ? C_RIGHT_TEXT : C_WRONG_TEXT);
        if (!right) c = C_DIM;
    }
    var worth = "+" + (answered_right() || mode != M_ANSWER ? points : 0);
    print(worth, 156 - textw(worth), 13, c);
}

function draw_tick(x, y) {
    line(x, y + 3, x + 2, y + 5, C_WHITE);
    line(x + 2, y + 5, x + 6, y + 1, C_WHITE);
}

function draw_cross(x, y) {
    line(x + 1, y + 1, x + 5, y + 5, C_WHITE);
    line(x + 5, y + 1, x + 1, y + 5, C_WHITE);
}

function draw_rows() {
    var r = deal.rounds[rd];
    font(FONT.WORLD);
    for (var i = 0; i < deal.CHOICES; i++) {
        var y = 23 + i * 19, bg = C_ROW, fg = C_ROW_TEXT, mark = 0;
        if (mode == M_ANSWER) {
            bg = C_ROW_FADED;
            fg = C_FADED;
            if (i == r.right) {
                bg = C_RIGHT;
                fg = C_WHITE;
                mark = 1;
            } else if (i == picked) {
                bg = C_WRONG;
                fg = C_WHITE;
                mark = 2;
            }
        } else if (paused) {
            bg = i == sel ? C_SEL_FADED : C_ROW_FADED;
            fg = i == sel ? C_SEL_FADED_TEXT : C_FADED;
        } else if (i == sel) {
            bg = C_SEL;
            fg = C_WHITE;
        }
        rectfill(4, y, 155, y + 16, bg);
        if (mark == 1) draw_tick(146, y + 5);
        if (mark == 2) draw_cross(146, y + 5);
        print(mark ? fits_marked[i] : fits[i], 8, y + 3, fg);
    }
    font(FONT.CAPS);
}

// Over the faded rows while the round is paused
function draw_paused() {
    rectfill(20, 51, 139, 88, C_SEL);
    rectfill(21, 52, 138, 87, C_ROW);
    rectfill(28, 64, 30, 74, C_TEXT);
    rectfill(33, 64, 35, 74, C_TEXT);
    font(FONT.WORLD);
    print("Paused", 42, 55, C_TEXT);
    font(FONT.CAPS);
    print("THIS ROUND IS LOST", 42, 70, C_WRONG_TEXT);
    print("PLAY TO GO ON", 42, 78, C_DIM);
}

function draw_round() {
    draw_head();
    draw_timer();
    draw_rows();
    if (paused) draw_paused();
}

var star_x = [0, 1, 4, 2, 3, 0, -3, -2, -4, -1];
var star_y = [-4, -1, -1, 1, 4, 2, 4, 1, -1, -1];
var star_xs = new Int32Array(10), star_ys = new Int32Array(10);

function draw_star(x, y, c) {
    for (var i = 0; i < 10; i++) {
        star_xs[i] = x + star_x[i];
        star_ys[i] = y + star_y[i];
    }
    polyfill(star_xs, star_ys, 10, c);
}

// The score, each round's share of it, and the best
function draw_result() {
    var total = "" + points_total, out_of = " OF " + deal.ROUNDS * ROUND_POINTS;
    var x = flr((160 - textw(total) - textw(out_of)) / 2);
    centred("MUSIC QUIZ", 14, C_DIM);
    print(total, x, 28, C_TEXT);
    print(out_of, x + textw(total), 28, C_DIM);

    // the rounds get harder left to right, and the strip says so
    for (var i = 0; i < deal.ROUNDS; i++) {
        var tx = 11 + i * 14, s = was_right[i] ? "" + earned[i] : "-";
        rectfill(tx, 46, tx + 11, 57, was_right[i] ? C_RIGHT : C_MISS);
        print(s, tx + flr((13 - textw(s)) / 2), 50,
              was_right[i] ? C_WHITE : C_WRONG_MARK);
    }
    print("EASIER", 11, 61, C_FADED);
    print("HARDER", 149 - textw("HARDER"), 61, C_FADED);

    if (record) {
        var bw = 6 + 9 + 3 + textw("NEW BEST") + 6, bx = flr((160 - bw) / 2);
        rectfill(bx, 72, bx + bw - 1, 84, C_BAR);
        draw_star(bx + 10, 78, C_BACK);
        print("NEW BEST", bx + 18, 76, C_BACK);
    } else {
        centred("BEST " + best, 76, C_DIM);
    }

    // what the two keys do, along the foot
    line(4, 104, 155, 104, C_TROUGH);
    print("SELECT", 4, 110, C_TEXT);
    print("PLAY AGAIN", 4 + textw("SELECT "), 110, C_DIM);
    print("LEAVE", 156 - textw("LEAVE"), 110, C_DIM);
    print("MENU", 156 - textw("LEAVE ") - textw("MENU"), 110, C_TEXT);
}
