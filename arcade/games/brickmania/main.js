// Brick Mania: break every brick with a ball and a paddle.
//
// The game is split across files, one subject each; from here they are
// called by file name, as bricks.hit() or fx.shake():
//
//   main.js     the screen's layout, the game's states, and init, update
//               and draw, which the Arcade calls
//   levels.js   the ten levels, as rows of letters
//   bricks.js   the wall: its bricks, how they are hit, crack and break
//   balls.js    the balls: moving, bouncing, and what they hit
//   paddle.js   the paddle: steering it with the wheel or the buttons
//   powers.js   power-ups: the capsules, what each does, the laser
//   fx.js       effects: sparks, falling debris, rings, floating text,
//               screen shake and hit-stop
//   hud.js      the score, lives and power-up timers along the top
//   title.js    the title screen's logo and the balls bouncing behind it
//
// Controls: turn the wheel to move the paddle (or press Prev and Next);
// Select serves the ball, lets go of a caught one, and fires the laser.
// The pause menu's Options choose Spin or Dial steering.

// ---- the screen's layout -------------------------------------------------
// The screen is 160 x 120. A 9-pixel strip along the top holds the score;
// below it, the playfield, walled on the left, right and top.

const HUD_H = 9;                // the strip along the top
const FIELD_L = 4;              // the playfield's inside edges: a ball
const FIELD_R = 156;            // keeps between FIELD_L and FIELD_R,
const FIELD_T = 11;             // and below FIELD_T
const FIELD_B = 120;            // a ball below this is lost

const BRICK_W = 12, BRICK_H = 6;    // a brick's cell; its picture is 11 x 5
const COLS = 12, ROWS = 9;          // the wall's grid
const WALL_X = 8, WALL_Y = 18;      // the wall's top left corner

const PADDLE_Y = 110;           // the paddle's top edge
const SHIELD_Y = 117;           // where the Shield power-up's line is

// ---- the sheet -------------------------------------------------------------
// Where each picture is on sheet.png. Bricks and capsules sit in 16 x 8
// slots, eight to a row; see bricks.slot() and powers.draw().

const SHEET_PADDLE_Y = 24;      // three 24 x 4 paddles, 32 apart:
const SHEET_PADDLE_LASER = 32;  //   normal at x 0, laser at 32,
const SHEET_PADDLE_STICKY = 64; //   sticky at 64
const SHEET_BALL_Y = 32;        // 4 x 4 balls, 8 apart: normal, fire,
                                // sticky, slow
                                // debris pieces are sprites 80-85, at
                                // y 40, drawn in colours 1, 2, 3: fx.js

// ---- the colours (palettes/game.gpl) ---------------------------------------

const C_BACKGROUND = 49, C_STAR_DIM = 50, C_STAR = 51;
const C_FRAME = 59, C_FRAME_HI = 60;
const C_WHITE = 7, C_GREY = 6, C_DARK = 5;
const C_LASER = 53, C_SHIELD = 54, C_FIRE = 55, C_GOLD = 56;
const C_SLOW = 57, C_STICKY = 58;
const C_SHARDS = 61;            // the shield's shards: 61-63, light to dark

// ---- the game's states -----------------------------------------------------
// TITLE -> INTRO (the level's name) -> SERVE (ball on the paddle) ->
// PLAY -> LOST (a life gone) or CLEAR (the wall broken) -> ... -> OVER

const ST_TITLE = 0, ST_INTRO = 1, ST_SERVE = 2, ST_PLAY = 3;
const ST_LOST = 4, ST_CLEAR = 5, ST_OVER = 6;

const INTRO_TICKS = 50;         // how long a level's name shows
const LOST_TICKS = 40;          // the pause after losing a life
const CLEAR_TICKS = 75;         // the level-clear celebration
const OVER_TICKS = 60;          // before Game Over takes Select

// A brick's points show this far above its middle
const TEXT_ABOVE = 10;

// Combo: each brick broken between touches of the paddle moves one step
// along this list; the score for a brick is multiplied by the step's value
var MULTIPLIERS = [1, 2, 3, 4, 5, 6, 8];

var state = ST_TITLE;
var state_ticks = 0;            // ticks spent in the current state
var level = 1;                  // 1 to 10, then round again, faster
var points = 0;                 // the score ("score" is the Arcade's own)
var lives = 3;
var combo = 0;                  // index into MULTIPLIERS
var spin = true;                // steering: Spin (true) or Dial

// ---- init, update and draw -----------------------------------------------

function init() {
    spin = opt.get(OPT.control) == 0;
    fx.setup();
    bricks.setup();
    title.setup();
}

function update() {
    if (opt.changed()) spin = opt.get(OPT.control) == 0;
    state_ticks++;

    // Effects keep moving whatever the state, so debris keeps falling
    // during a pause; fx.update() also counts down screen shake. In the
    // level-clear celebration's slow motion they move every other tick.
    // Hit-stop freezes everything else for a moment after a big impact.
    if (!slow_motion() || state_ticks % 2 == 0) fx.update();
    if (fx.stopped()) return;

    if (state == ST_TITLE) update_title();
    else if (state == ST_INTRO) update_intro();
    else if (state == ST_SERVE || state == ST_PLAY) update_play();
    else if (state == ST_LOST) update_lost();
    else if (state == ST_CLEAR) update_clear();
    else if (state == ST_OVER) update_over();
}

function draw() {
    // the world, shaken; then the HUD, which never shakes
    camera(fx.shake_x, fx.shake_y);
    draw_background();
    if (state == ST_TITLE) title.draw();
    else bricks.draw();
    fx.draw_behind();           // debris falls behind the paddle and balls
    powers.draw();
    paddle.draw();
    balls.draw();
    fx.draw_front();            // sparks, rings and floating text on top
    camera();
    hud.draw();
    draw_messages();
}

// ---- each state --------------------------------------------------------------

function set_state(s) {
    state = s;
    state_ticks = 0;
}

function update_title() {
    title.update();
    if (btnp(4)) new_game();
}

function new_game() {
    points = 0;
    lives = opt.get(OPT.lives);
    level = 1;
    start_level();
}

// A level from the start: its wall, a fresh paddle, a ball to serve
function start_level() {
    levels.load(level);
    powers.reset();
    paddle.reset();
    balls.reset();
    combo = 0;
    set_state(ST_INTRO);
}

function update_intro() {
    paddle.update();
    if (state_ticks >= INTRO_TICKS || btnp(4)) serve();
}

// A new ball on the paddle, waiting for Select
function serve() {
    balls.reset();
    balls.hold(balls.add(paddle.x, PADDLE_Y - 2, 0, 0));
    set_state(ST_SERVE);
}

function update_play() {
    paddle.update();
    // Select lets go of a held ball first; with none held it fires the
    // laser, if the paddle has one
    if (btnp(4) || btnp(3)) {
        if (balls.release()) set_state(ST_PLAY);
        else powers.fire();
    }
    bricks.update();
    balls.update();
    powers.update();
}

// Called by balls.js when the last ball falls out of the bottom
function ball_lost() {
    lives--;
    combo = 0;
    powers.reset();
    sfx(SFX.lost);
    fx.shake(2, 6);
    set_state(lives > 0 ? ST_LOST : ST_OVER);
    if (state == ST_OVER) game_over();
}

function update_lost() {
    paddle.update();
    if (state_ticks >= LOST_TICKS) {
        paddle.reset_width();
        serve();
    }
}

// Called by bricks.js when the last breakable brick goes
function level_cleared() {
    set_state(ST_CLEAR);
    balls.reset();              // the ball's work is done
    sfx(SFX.clear);
    fx.shake(3, 6);
    fx.stop(3);
    fx.celebrate();
}

// The celebration, by the tick (the spec's timeline):
//   0-4 debris falls; 5-24 slow motion; 25 CLEAR shows; 40 the sparks
//   are drawn to the centre; 60 the screen is wiped; 75 the next level.
function update_clear() {
    paddle.update();
    bricks.update();            // the last brick's flash fades
    if (state_ticks == 40) fx.gather(80, 60);
    if (state_ticks >= CLEAR_TICKS) {
        level++;
        start_level();
    }
}

function slow_motion() {
    return state == ST_CLEAR && state_ticks >= 5 && state_ticks < 25;
}

// The score's place on the board, 0 when it did not make the ten: Game
// Over says so, rather than leaving the game for the player's own table
var over_rank = 0;

function game_over() {
    balls.reset();
    over_rank = score.submit(BOARD.points, points);
}

function update_over() {
    if (state_ticks >= OVER_TICKS && btnp(4)) set_state(ST_TITLE);
}

// ---- scoring ----------------------------------------------------------------

// Points for a broken brick at x, y: multiplied by the combo (and doubled
// while the paddle is shrunk), shown above where it broke, out of the
// ball's way, with the raised combo beside them
function award(base, x, y) {
    var n = base * MULTIPLIERS[combo] * (powers.shrunk() ? 2 : 1);
    var gained = "" + n;
    points += n;
    var ty = max(y - TEXT_ABOVE, FIELD_T + 1);
    if (!fx.busy()) fx.text(gained, x, ty, C_WHITE);
    if (combo < MULTIPLIERS.length - 1) {
        combo++;
        fx.text("X" + MULTIPLIERS[combo], x + textw(gained) + 2, ty, C_GOLD);
    }
}

// A paddle touch ends the run of bricks
function combo_reset() {
    combo = 0;
}

// ---- drawing ------------------------------------------------------------------

// The playfield: a dark sky with a few slow stars, and its frame
function draw_background() {
    cls(C_BACKGROUND);
    fx.draw_stars();
    rectfill(FIELD_L - 2, FIELD_T - 2, FIELD_R + 1, FIELD_T - 1, C_FRAME);
    rectfill(FIELD_L - 2, FIELD_T - 2, FIELD_L - 1, 119, C_FRAME);
    rectfill(FIELD_R, FIELD_T - 2, FIELD_R + 1, 119, C_FRAME);
    line(FIELD_L - 2, FIELD_T - 2, FIELD_R + 1, FIELD_T - 2, C_FRAME_HI);
}

// The words over the playfield for the states that have them
function draw_messages() {
    if (state == ST_TITLE) {
        var best = score.best(BOARD.points);
        if (best != null) centre("BEST " + best, 82, C_GREY);
        if (frame() % 40 < 28) centre("SELECT TO PLAY", 94, C_GOLD);
    } else if (state == ST_INTRO) {
        centre("LEVEL " + level, 80, C_WHITE);
        centre(levels.name(level), 88, C_GREY);
    } else if (state == ST_SERVE) {
        if (frame() % 40 < 28) centre("SELECT", 96, C_GREY);
    } else if (state == ST_LOST) {
        centre("BALL LOST", 80, C_WHITE);
    } else if (state == ST_CLEAR && state_ticks >= 25) {
        font(FONT.CAPS, 2);
        centre("CLEAR", 50, frame() % 8 < 4 ? C_WHITE : C_GOLD);
        font(FONT.CAPS);
        if (state_ticks >= 60) wipe(state_ticks - 60);
    } else if (state == ST_OVER) {
        font(FONT.CAPS, 2);
        centre("GAME OVER", 48, C_WHITE);
        font(FONT.CAPS);
        centre("" + points, 70, C_GOLD);
        if (over_rank == 1) centre("NEW HIGH SCORE", 78, C_WHITE);
        else if (over_rank > 1) centre("HIGH SCORE NO. " + over_rank, 78, C_WHITE);
        if (state_ticks >= OVER_TICKS) centre("SELECT", 90, C_GREY);
    }
}

// The end of a level: bars closing in from the top and bottom, over the
// last 15 ticks
function wipe(step) {
    var h = flr(step * 60 / 15);
    rectfill(0, HUD_H, 159, HUD_H + h, C_BACKGROUND);
    rectfill(0, 119 - h, 159, 119, C_BACKGROUND);
}

// Text centred across the screen at y
function centre(text, y, c) {
    print(text, 80 - textw(text) / 2, y, c);
}
