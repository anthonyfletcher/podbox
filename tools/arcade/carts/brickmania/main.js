// Brick Mania, phase 1: one ball, plain and tough bricks, three levels,
// score and lives in the side panels, the best score kept. The core only:
// every line the cart's own.
//
// The playfield is 128 wide between two 16-pixel panels. The wheel is the
// paddle: Spin moves it by the turn, Dial puts it where the finger is on
// the top half of the wheel; PREV and NEXT move it too. SELECT launches.

const LEFT = 16, RIGHT = 144, TOP = 8;      // the walls' inner edges
const WALL_Y = 12, ROWS = 12, COLS = 16;    // bricks: 8x4 from y 12
const PADDLE_Y = 112, PADDLE_W = 20;
const START_SPEED = 1.3, SPEED_STEP = 0.02, SPEED_CAP = 2.6;

// Brick kinds: 0 none, 1-8 plain colours, 9 tough, 10 tough and cracked
var points = new Int32Array(11);
var next = new Uint8Array(11);              // what a kind becomes when hit
var colour = new Uint8Array(11);

var levels = [
    ["................",
     "................",
     "8888888888888888",
     "9999999999999999",
     "aaaaaaaaaaaaaaaa",
     "bbbbbbbbbbbbbbbb",
     "cccccccccccccccc",
     "eeeeeeeeeeeeeeee"],
    ["................",
     "TT..TT..TT..TT..",
     "88..99..aa..bb..",
     "..cc..ee..88..99",
     "TT..TT..TT..TT..",
     "aa..bb..cc..ee..",
     "..88..99..aa..bb",
     "................",
     "TTTTTTTTTTTTTTTT"],
    [".......TT.......",
     "......T88T......",
     ".....T8998T.....",
     "....T89aa98T....",
     "...T89abba98T...",
     "..T89abccba98T..",
     "...T89abba98T...",
     "....T89aa98T....",
     ".....T8998T.....",
     "......T88T......",
     ".......TT......."]
];

var wall = new Uint8Array(ROWS * COLS), bricks = 0, picture = null;
var level = 1, score = 0, best = 0, lives = 3, combo = 0;
var paddle = 70, spin = true, turned = 0;
var bx = 0, by = 0, bdx = 0, bdy = 0, speed = START_SPEED, held = true;
var state = 0;                              // 0 play, 1 lost a life, 2 over
var wait = 0;

function init() {
    // plain bricks score by colour, 8 (red) the most
    var plain = [8, 9, 10, 11, 12, 14, 13, 2];
    for (var k = 1; k <= 8; k++) {
        colour[k] = plain[k - 1];
        points[k] = 10 * (9 - k);
        next[k] = 0;
    }
    colour[9] = 6; points[9] = 20; next[9] = 10;
    colour[10] = 5; points[10] = 30; next[10] = 0;
    best = dget(0);
    picture = img(128, ROWS * 4);
    menuitem(1, "Control: Spin", control);
    start_level();
}

function control(button) {
    spin = !spin;
    menuitem(1, spin ? "Control: Spin" : "Control: Dial", control);
    return true;
}

// The level's wall, from its rows of characters: . none, 8-e a plain
// colour (by palette number), T tough
function start_level() {
    var rows = levels[(level - 1) % levels.length];
    bricks = 0;
    for (var r = 0; r < ROWS; r++)
        for (var c = 0; c < COLS; c++) {
            var ch = r < rows.length ? rows[r].charCodeAt(c) : 46;
            var k = kind(ch);
            wall[r * COLS + c] = k;
            if (k) bricks++;
        }
    draw_wall();
    speed = START_SPEED + (level - 1) * 0.1;
    hold_ball();
}

// A level's character: . none, T tough, 8 9 a b c e the plain colours
function kind(ch) {
    if (ch == 84) return 9;                 // T
    if (ch == 56) return 1;                 // 8
    if (ch == 57) return 2;                 // 9
    if (ch >= 97 && ch <= 99) return ch - 94;   // a b c
    if (ch == 101) return 6;                // e
    return 0;
}

function draw_wall() {
    target(picture);
    cls(0);
    for (var r = 0; r < ROWS; r++)
        for (var c = 0; c < COLS; c++)
            draw_brick(r, c);
    target(SCREEN);
}

function draw_brick(r, c) {
    var k = wall[r * COLS + c], x = c * 8, y = r * 4;
    if (!k) {
        rectfill(x, y, x + 7, y + 3, 0);
        return;
    }
    rectfill(x, y, x + 7, y + 3, colour[k]);
    line(x, y + 3, x + 7, y + 3, 0);
    line(x + 7, y, x + 7, y + 3, 0);
    if (k == 10) {
        pset(x + 2, y + 1, 0);
        pset(x + 3, y + 2, 0);
        pset(x + 5, y + 1, 0);
    }
}

function hold_ball() {
    held = true;
    bx = paddle + PADDLE_W / 2;
    by = PADDLE_Y - 3;
    combo = 0;
}

function launch() {
    held = false;
    aim(0.5 + (rnd(20) - 10) / 100.0);
}

// Off the paddle at 30 to 150 degrees by where it struck: t 0 is the
// paddle's left end. sin is inverted, so these point up the screen.
function aim(t) {
    var a = (150 - t * 120) / 360.0;
    bdx = cos(a) * speed;
    bdy = sin(a) * speed;
}

function update() {
    move_paddle();
    if (state == 1 || state == 2) {
        if (wait > 0) wait--;
        else if (btnp(4)) {
            if (state == 2) {
                level = 1;
                score = 0;
                lives = 3;
                start_level();
            } else
                hold_ball();
            state = 0;
        }
        return;
    }
    if (held) {
        bx = paddle + PADDLE_W / 2;
        if (btnp(4)) launch();
        return;
    }
    // in steps no longer than a brick is tall, so none is passed through
    var steps = flr(max(abs(bdx), abs(bdy)) / 2) + 1;
    for (var s = 0; s < steps && !held; s++)
        step_ball(bdx / steps, bdy / steps);
}

function move_paddle() {
    turned += wheel();
    if (spin) {
        paddle += turned / 2.0;
        turned = 0;
    } else {
        var p = wheelpos();
        // the top half of the wheel, nine o'clock (72) round to three (24)
        if (p >= 72 || (p >= 0 && p <= 24)) {
            var t = p >= 72 ? p - 72 : p + 24;
            paddle = LEFT + t * (RIGHT - LEFT - PADDLE_W) / 48.0;
        }
        turned = 0;
    }
    if (btn(0)) paddle -= 3;
    if (btn(1)) paddle += 3;
    paddle = mid(LEFT, paddle, RIGHT - PADDLE_W);
}

function step_ball(dx, dy) {
    var ox = bx, oy = by;
    bx += dx;
    by += dy;
    // the walls and the ceiling
    if (bx < LEFT) { bx = LEFT; bdx = abs(bdx); sfx(SFX.wall); }
    if (bx > RIGHT - 2) { bx = RIGHT - 2; bdx = -abs(bdx); sfx(SFX.wall); }
    if (by < TOP) { by = TOP; bdy = abs(bdy); sfx(SFX.wall); }
    // the paddle
    if (bdy > 0 && by + 2 >= PADDLE_Y && oy + 2 <= PADDLE_Y + 1 &&
        bx + 2 > paddle && bx < paddle + PADDLE_W) {
        by = PADDLE_Y - 2;
        aim((bx + 1 - paddle) / PADDLE_W);
        combo = 0;
        sfx(SFX.paddle);
        return;
    }
    if (by > 122) {
        lose_life();
        return;
    }
    // the brick under the ball's centre, if any
    var c = flr((bx + 1 - LEFT) / 8), r = flr((by + 1 - WALL_Y) / 4);
    if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return;
    var k = wall[r * COLS + c];
    if (!k) return;
    // which way it came in decides which way it turns
    var oc = flr((ox + 1 - LEFT) / 8);
    if (oc != c) { bdx = -bdx; bx = ox; }
    else { bdy = -bdy; by = oy; }
    hit(r, c, k);
}

function hit(r, c, k) {
    score += points[k];
    wall[r * COLS + c] = next[k];
    if (!next[k]) bricks--;
    target(picture);
    draw_brick(r, c);
    target(SCREEN);
    sfxp(SFX.brick, min(combo, 24));
    combo++;
    speed = min(speed + SPEED_STEP, SPEED_CAP);
    var len = sqrt(bdx * bdx + bdy * bdy);
    if (len > 0) { bdx = bdx * speed / len; bdy = bdy * speed / len; }
    if (score > best) best = score;
    if (bricks == 0) {
        level++;
        dset(0, best);
        start_level();
    }
}

function lose_life() {
    lives--;
    held = true;
    sfx(SFX.lost);
    wait = 20;
    if (lives <= 0) {
        state = 2;
        dset(0, best);
    } else
        state = 1;
}

function draw() {
    cls(0);
    // the walls
    rectfill(LEFT - 2, TOP - 2, RIGHT + 1, TOP - 1, 5);
    rectfill(LEFT - 2, TOP - 2, LEFT - 1, 120, 5);
    rectfill(RIGHT, TOP - 2, RIGHT + 1, 120, 5);
    blit(picture, LEFT, WALL_Y);
    rectfill(paddle, PADDLE_Y, paddle + PADDLE_W - 1, PADDLE_Y + 2, 7);
    rectfill(paddle + 2, PADDLE_Y + 1, paddle + PADDLE_W - 3, PADDLE_Y + 1, 12);
    if (state != 2) rectfill(bx, by, bx + 1, by + 1, 10);
    draw_panels();
    if (state == 1) print("READY", 70, 70, 7);
    if (state == 2) {
        print("GAME OVER", 62, 64, 8);
        print("SELECT", 68, 74, 6);
    }
    if (held && state == 0) print("SELECT", 68, 90, 6);
}

// Left: level, score (two lines of three digits) and lives; right: best
function draw_panels() {
    print("LV", 2, 2, 6);
    print(level, 2, 9, 7);
    print("PTS", 1, 22, 6);
    digits(score, 1, 29);
    print("BL", 2, 48, 6);
    for (var i = 0; i < lives && i < 8; i++)
        rectfill(2, 56 + i * 4, 11, 57 + i * 4, 7);
    print("TOP", 147, 2, 6);
    digits(best, 147, 9);
}

function digits(n, x, y) {
    var hi = flr(n / 1000), lo = n % 1000;
    print(pad(hi), x, y, 7);
    print(pad(lo), x, y + 7, 7);
}

function pad(n) {
    if (n < 10) return "00" + n;
    if (n < 100) return "0" + n;
    return "" + n;
}
