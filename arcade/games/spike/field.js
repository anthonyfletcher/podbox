// The field: the course, the player, the band above them.
//
// Positions are worked in Spike's own coordinates -- 320 by 240, a cell 32
// pixels -- so every number is the one the player's Spike used. The
// course is sprites from sheet.png, placed at half those numbers. Spike
// and the moments on his cell -- a stomp, an eaten diamond, his tail -- are
// line art drawn under a camera zoomed to one half, so each stroke lands a
// screen pixel wide. The band is drawn at the screen's own size.
//
// It reads the frame from main.js's globals (st, phase, strong and the
// rest) and keeps no state but the jump's tail.
//
// Parts: the colours; the sprites; what happens on the player's cell; the
// player; the band; the frame.

// ---- the geometry ------------------------------------------------------------

const CELL_PX = 32, LEVEL = 20;
const BODY_PX = 44;
const FIELD_H = LEVEL * 5 + BODY_PX;            // 144
const BAND_H = (240 - FIELD_H) / 2;             // 48
const HUD_DROP = 10;
const HUD_H = BAND_H + HUD_DROP;                // 58: the rule under the band
const GROUND_Y = HUD_H + FIELD_H;               // 202
const PLAYER_COL = 2;
const PLAYER_X = PLAYER_COL * CELL_PX + CELL_PX / 2;  // 80
const ROWS_PER_BEAT = 16;
const CAP_H = 5;                // FONT.CAPS's digits, without the room under them

// What the body has to fit: a creature's head (which a landing comes down
// on), a block, a switch, a spring
const CR_BODY = 11, CR_UP = 12;
const CR_HEAD8 = ((CR_UP + CR_BODY + 3) * 256) / LEVEL;

const BLOCK_H = 14, BLOCK_LIFT = LEVEL * 2;
const SW_UP = 6;
const SPR_TALL = 14, SPR_LOW = 4, SPR_KICK = 19;
const FLY_LEAD = 4;

// The tail a jump leaves: five dashes back along the arc, held in the world
const TRAIL = 8, TRAIL_JUMP = 5, TRAIL_STEP = 24, TRAIL_MS = 900, TRAIL_FALL = 1400;
const FALL_STEP = 16;

// ---- the colours -------------------------------------------------------------

// The album's colours, made into shades in palette entries of their own:
// the sprites and the art are drawn in these entries, and take on each
// album's colours when they are set. The background: the paper and two
// shades of it toward the ink. The foreground: the ink and three shades of
// it toward the paper. The accent: the album's vivid colour, a shade toward
// the paper and one toward the ink. palettes/spike.gpl holds one album's
// worth, to draw with.
const PAPER = 16, PAPER2 = 17, PAPER3 = 18;
const INK = 19, INK2 = 20, INK3 = 21, INK4 = 22;
const ACCENT = 23, ACCENT2 = 24, ACCENT3 = 25;

const SHADE = 8;            // tones between one shade and the next

var heard = new Int32Array(3);

// Set from what is playing; about 8,000 units, once a track
function colours() {
    playback.colors(heard);
    var paper = heard[0], ink = heard[1];
    var t = color.tone(paper), k = color.tone(ink);
    var away = t < k ? SHADE : -SHADE;
    setpal(PAPER, paper);
    setpal(PAPER2, color.toned(paper, t + away));
    setpal(PAPER3, color.toned(paper, t + away * 2));
    setpal(INK, ink);
    setpal(INK2, color.toned(ink, k - away));
    setpal(INK3, color.toned(ink, k - away * 2));
    setpal(INK4, color.toned(ink, k - away * 3));
    // the vivid colour, unless it would vanish against the paper
    var accent = heard[2];
    if (color.contrast(accent, paper) < 2.0)
        accent = color.toned(accent, t < 50 ? 75 : 30);
    var a = color.tone(accent);
    setpal(ACCENT, accent);
    setpal(ACCENT2, color.toned(accent, a - away));
    setpal(ACCENT3, color.toned(accent, a + away));
}

// ---- the world -----------------------------------------------------------------

var trailWorld = new Int32Array(TRAIL);     // where each dash is, 8.8 cells
var trailY = new Int32Array(TRAIL);
var trailN = 0;
var trailFalling = false;
var trailAt = 0;

function reset() {
    trailN = 0;
    trailFalling = false;
}

function levelY(level8) {
    return GROUND_Y - ((LEVEL * level8) >> 8);
}

// How far into the cell the world has scrolled, 0..256
function scroll8() {
    if (deathPhase >= 0)
        return deathScroll;
    if (frozen)
        return 0;
    return pose.ease(phase);
}

function scrollPx() {
    return (scroll8() * CELL_PX) >> 8;
}

// ---- the sprites -------------------------------------------------------------

// Where each thing is on the sheet. A cell's pieces are 16 by 8 with the
// surface on their top row; the switch is 16 by 8 with its surface on the
// bottom row; a diamond and the block 8 by 8; the animals and the spring
// 16 by 16, standing on the row under them. Each has its beat's two frames.
const S_GROUND = 0, S_GROUND_HOOK = 2, S_GROUND_END = 4, S_GROUND_BOTH = 6;
const S_VOID = 8, S_PROMISE = 10, S_PLATFORM = 12, S_PLATFORM_B = 14;
const S_SWITCH = 16, S_SWITCH_HALF = 18, S_SWITCH_FLAT = 20;
const S_DIAMOND = 22, S_DIAMOND_B = 23, S_BLOCK = 24;
const S_CREATURE = 32, S_CREATURE_B = 34, S_SPIKED = 36, S_SPIKED_B = 38;
const S_FLYER = 40, S_FLYER_B = 42, S_SPRING = 44, S_SPRING_B = 46;
const S_SPRING_LOW = 64, S_SPRING_MID = 66, S_SPRING_KICK = 68;
// The stomp's ten frames, 40 by 16, from here: three to a pair of rows
const S_STOMP = 96;
// ...and the eaten diamond's five, 16 by 16, along the bottom
const S_EATEN = 224;

// The screen row of a level's surface
function surfaceY(level) {
    return (GROUND_Y - LEVEL * level) >> 1;
}

// The rows of a spring launching off its cap: squashed, coming up, kicked
// past rest; the body reads its height from springTop
function springRow(c) {
    if (st.motion != world.ARC_SPRING || c != st.beat)
        return -1;
    return (phase * ROWS_PER_BEAT) / pose.PHASE;
}

function springTop(c) {
    var r = springRow(c);
    return r < 0 ? SPR_TALL : r == 0 ? SPR_LOW : r == 1 ? (SPR_LOW + SPR_TALL) / 2 : r == 2 ? SPR_KICK : SPR_TALL;
}

// One cell's sprites, its left edge at screen x: e is its bits, and lv the
// surfaces there now in the cells either side of it as well
function cellSprites(c, x, e, lv, before, after) {
    var soon = (e & gen.C_SWM) ? world.promised(c) : 0, level;
    var g = surfaceY(0);
    if (lv & 1) {
        var hook = !(before & 1), end = !(after & 1);
        spr(hook ? (end ? S_GROUND_BOTH : S_GROUND_HOOK) : (end ? S_GROUND_END : S_GROUND), x, g, 2, 1);
    } else {
        spr(S_VOID, x, g, 2, 1);
        if (soon & 1)
            spr(S_PROMISE, x, g, 2, 1);
    }
    for (level = 1; level < world.LEVELS; level++) {
        if (lv & (1 << level))
            spr(strong ? S_PLATFORM : S_PLATFORM_B, x, surfaceY(level), 2, 1);
        else if (soon & (1 << level))
            spr(S_PROMISE, x, surfaceY(level), 2, 1);
    }
    level = (e & gen.C_SWF) ? world.hasSwitch(c) : -1;
    if (level >= 0) {
        var s = S_SWITCH_FLAT;
        if (world.switchAt(c) >= 0)
            s = S_SWITCH;
        else if (st.threw && c == st.beat && (phase * ROWS_PER_BEAT) / pose.PHASE < 4)
            s = S_SWITCH_HALF;
        spr(s, x, surfaceY(level) - 7, 2, 1);
    }
    level = (e & gen.C_DIF) ? world.diamond(c) : -1;
    if (level >= 0)
        spr(strong ? S_DIAMOND : S_DIAMOND_B, x + 4, surfaceY(level) - 9);
    if (e & gen.C_BLF) {
        // seen to travel over the beat before the one it matters on
        var down = GROUND_Y - BLOCK_H, up = down - BLOCK_LIFT;
        var was = world.blockUp(c, st.beat) ? up : down;
        var now = world.blockUp(c, st.beat + 1) ? up : down;
        var top = was + ((now - was) * scroll8()) / pose.PHASE;
        spr(S_BLOCK, x + 4, (top >> 1) - 1);
    }
    if (e & gen.C_SPR) {
        var r = springRow(c);
        var n = r < 0 || r > 2 ? (strong ? S_SPRING : S_SPRING_B)
                               : r == 0 ? S_SPRING_LOW : r == 1 ? S_SPRING_MID : S_SPRING_KICK;
        spr(n, x, g - 16, 2, 2);
    }
    level = (e & gen.C_CRF) ? world.creature(c) : -1;
    if (level >= 0) {
        var k = (e & gen.C_SPK) ? (strong ? S_SPIKED : S_SPIKED_B) : (strong ? S_CREATURE : S_CREATURE_B);
        spr(k, x, surfaceY(level) - 16, 2, 2);
    }
}

// The flyer, kicking twice a beat, a column of its own away
function flyerSprite(x, level) {
    var kick = strong != (phase >= pose.PHASE / 2);
    spr(kick ? S_FLYER : S_FLYER_B, x, surfaceY(level) - 15, 2, 2);
}

// ---- what happens on the player's cell -----------------------------------------

// Moments rather than things, each lasting part of a beat, from the sheet

// An eaten diamond, its edges coming apart over the first five rows of the
// beat: 16 by 16 frames, the diamond's middle on theirs. x is the cell's
// left edge on the screen.
function drawEaten(x, level) {
    var r = (phase * ROWS_PER_BEAT) / pose.PHASE;
    if (r < 5)
        spr(S_EATEN + 2 * r, x, surfaceY(level) - 14, 2, 2);
}

// A creature stomped, in its cell, going by with the course: its own sprite
// for the first row of the beat -- the walker standing tall, the height a
// landing comes down on, and the flyer in whichever of its kicks it was in
// -- squashed over the next three, bursting from the third, then coming
// apart over two more. The frames are 40 by 16 with the cell's middle on
// their 20th pixel, each standing as its animal's sprites do: the walker's
// and the pieces on the row under them, the flyer's on their bottom row.
// cellX is the cell's left edge on the screen.
var stompWalker = [0, S_STOMP + 5, S_STOMP + 10, S_STOMP + 32];
var stompFlyer = [0, S_STOMP + 42, S_STOMP + 64, S_STOMP + 69];
var stompApart = [S_STOMP + 74, S_STOMP + 96];

function drawStomp(cellX, level, flying) {
    var r = (phase * ROWS_PER_BEAT) / pose.PHASE, base = surfaceY(level);
    var x = cellX - 12;
    if (r >= 6)
        return;
    if (r == 0) {
        if (flying)
            flyerSprite(x + 12, level);
        else
            spr(S_CREATURE, x + 12, base - 16, 2, 2);
    } else if (r >= 4)
        spr(stompApart[r - 4], x, base - 16, 5, 2);
    else if (flying)
        spr(stompFlyer[r], x, base - 15, 5, 2);
    else
        spr(stompWalker[r], x, base - 16, 5, 2);
}

// The level of the flyer the cell at column c holds this beat, or -1: it
// meets the player at M on beat M, so at beat b it is at 2M - b
function flyerAt(c) {
    var b = st.beat;
    if (((c + b) & 1) != 0)
        return -1;
    var m = (c + b) / 2;
    if (m - b > FLY_LEAD || b - m > FLY_LEAD || !(gen.cell(m) & gen.C_FLY))
        return -1;
    return world.flyer(m);
}

// The course: sprites at the screen's size, then what is happening on the
// player's own cell in line art at the field's
// The columns on screen: one part off the left as the course scrolls, ten,
// and one coming on at the right; and the one past that, whose flyer is
// drawn a cell to the left of it
const COLUMNS = 320 / CELL_PX + 2;
var lives = new Int32Array(COLUMNS + 3);

function drawWorld() {
    var sc = scrollPx();
    var first = st.beat - PLAYER_COL - 1, i, c;
    camera();
    // what stands in each column now, and either side of them
    for (i = 0; i < COLUMNS + 2; i++) {
        c = first - 1 + i;
        var e = gen.cell(c);
        lives[i] = (e & gen.C_SWM) ? world.live(c) : e & gen.C_MASK;
    }
    for (i = 0; i <= COLUMNS; i++) {
        c = first + i;
        var x = (i - 1) * CELL_PX - sc;
        if (i < COLUMNS)
            cellSprites(c, x >> 1, gen.cell(c), lives[i + 1], lives[i], lives[i + 2]);
        var level = flyerAt(c);
        if (level >= 0)
            flyerSprite((x - sc) >> 1, level);
    }
    if (st.stomped)
        drawStomp((PLAYER_COL * CELL_PX - sc) >> 1, st.level < 0 ? 0 : st.level, world.flew(st.beat));
    if (st.got)
        drawEaten((PLAYER_COL * CELL_PX - sc) >> 1, st.gotLevel);
}

// ---- the player --------------------------------------------------------------

var body = pose.newPose();
var spare = pose.newPose();
var px = [0, 0, 0], py = [0, 0, 0];

// The body's pose for this instant, into body; returns its level, 8.8
function playerPose() {
    var level = st.level < 0 ? 0 : st.level, cl;
    if (deathPhase >= 0) {
        if (deathKind == DEATH_OUCH) {
            pose.ouch(body, deathPhase);
            if (st.motion == world.ARC_RISE)
                body.yo = body.yo - BLOCK_LIFT;
            else if (st.motion == world.ARC_FALL && st.to >= 0) {
                if (world.creature(st.beat + 1) == st.to)
                    return (st.to << 8) + CR_HEAD8;
                return st.to << 8;
            }
        } else
            pose.fall(body, deathPhase, deathKind == DEATH_AIR, strong);
        return level << 8;
    }
    if (st.motion != world.WALK) {
        // the pose keeps time; the height keeps the eased phase
        var uTime = phase >> 1, uWorld = pose.ease(phase) >> 1;
        var landStrong = strong;
        if (st.motion == world.ARC_FALL) {
            uTime += pose.PHASE / 2;
            uWorld += pose.PHASE / 2;
            landStrong = !strong;
        }
        pose.jump(body, uTime, landStrong);
        if (st.motion == world.ARC_FALL) {
            var over = 0;
            if (world.creature(st.beat + 1) == st.to || world.flyer(st.beat + 1) == st.to)
                over = CR_HEAD8;
            else if (world.switchAt(st.beat + 1) == st.to)
                over = (SW_UP * 256) / LEVEL;
            else if (st.to == 0 && world.spring(st.beat + 1))
                over = (SPR_TALL * 256) / LEVEL;
            if (over && st.to >= 0)
                return pose.arcLevel(st.from, st.to, uWorld, st.apex) + (over * phase) / pose.PHASE;
        }
        if (st.motion == world.ARC_SPRING) {
            var arc = pose.arcLevel(st.from, st.to, uWorld, st.apex);
            var cap = (springTop(st.beat) * 256) / LEVEL;
            return arc > cap ? arc : cap;
        }
        return pose.arcLevel(st.from, st.to, uWorld, st.apex);
    }
    if (st.landed)
        pose.landed(body, phase, strong, st.stomped ? CR_UP + CR_BODY : st.threw ? SW_UP : 0);
    else
        pose.hop(body, phase, strong);
    return pose.walkLevel(st.from, st.to, phase);
}

function triangle() {
    line(px[0], py[0], px[1], py[1], INK);
    line(px[1], py[1], px[2], py[2], INK);
    line(px[2], py[2], px[0], py[0], INK);
}

function drawPlayer() {
    var y = levelY(playerPose());
    pose.points(body, PLAYER_X, y, px, py);
    triangle();
    // ouch: three short lines off the point of contact
    if (deathPhase >= 0 && deathKind == DEATH_OUCH && deathPhase < pose.PHASE / 5) {
        var hx = PLAYER_X + 13, hy = y - 14;
        line(hx, hy, hx + 9, hy - 7, INK);
        line(hx + 1, hy + 7, hx + 11, hy + 5, INK);
        line(hx - 3, hy - 6, hx + 1, hy - 15, INK);
    }
    // impact ticks off the base corners
    if (st.landed && phase < pose.PHASE / 8 && deathPhase < 0) {
        line(px[0], py[0], px[0] - 4, py[0] - 3, INK);
        line(px[1], py[1], px[1] + 4, py[1] - 3, INK);
    }
}

// Coming back: growing out of the cell the run restarts from, drawn
// against that cell so the scroll brings it home
function drawDrop() {
    var surf = levelY(dropLevel << 8);
    var x = PLAYER_X + dropCells * CELL_PX - scrollPx();
    if (dropGrow < pose.PHASE)
        pose.sprout(spare, dropGrow, !strong);
    else
        pose.landed(spare, phase, strong, 0);
    pose.points(spare, x, surf, px, py);
    triangle();
}

// The tail: held along the arc while airborne, let go to fall after
function trailHold(world8, u) {
    trailN = 0;
    trailFalling = false;
    for (var i = 1; i <= TRAIL_JUMP; i++) {
        var back = u - i * TRAIL_STEP;
        if (back < 0)
            break;
        trailWorld[trailN] = world8 - i * TRAIL_STEP * 2;
        trailY[trailN] = levelY(pose.arcLevel(st.from, st.to, back, st.apex)) - 6;
        trailN++;
    }
}

function trailDrop(level) {
    var world8 = (st.beat << 8) + scroll8();
    var base = levelY(level << 8);
    var n = TRAIL_JUMP + level;
    trailN = 0;
    trailFalling = false;
    if (n > TRAIL)
        n = TRAIL;
    for (var i = 1; i <= n; i++) {
        var back = deathPhase - i * FALL_STEP;
        if (back < 0)
            break;
        pose.fall(spare, back, deathKind == DEATH_AIR, strong);
        trailWorld[trailN] = world8;
        trailY[trailN] = base + spare.yo - 6;
        trailN++;
    }
}

function trailStep() {
    if (!skipping) {
        if (deathPhase >= 0) {
            if (deathKind != DEATH_OUCH)
                trailDrop(st.level < 0 ? 0 : st.level);
            return;
        }
        if (st.motion != world.WALK) {
            var u = pose.ease(phase) >> 1;
            if (st.motion == world.ARC_FALL)
                u += pose.PHASE / 2;
            trailHold((st.beat << 8) + scroll8(), u);
            return;
        }
    }
    if (trailN > 0 && !trailFalling) {
        trailFalling = true;
        trailAt = nowMs;
    }
}

function trailDraw() {
    var camera8 = (st.beat << 8) + scroll8(), drop = 0;
    if (trailN == 0)
        return;
    if (trailFalling) {
        var age = nowMs - trailAt;
        if (age < 0 || age > TRAIL_MS) {
            trailN = 0;
            return;
        }
        drop = (age * age) / TRAIL_FALL;
    }
    for (var i = 0; i < trailN; i++) {
        var y = trailY[i] + drop;
        var surf = world.surface(trailWorld[i] >> 8, world.LEVELS - 1);
        if (surf >= 0) {
            var floorY = GROUND_Y - LEVEL * surf;
            if (y > floorY)
                y = floorY;
        } else if (y > GROUND_Y + 8)
            continue;
        var x = PLAYER_X + (((trailWorld[i] - camera8) * CELL_PX) >> 8);
        if (x > -4 && x <= 324)
            line(x - 2, y, x, y, INK2);
    }
}

// ---- the band ----------------------------------------------------------------

// The crown, h square, at x, y on the screen
function crown(x, y, h) {
    var half = h / 2, cx = x + half, dip = y + h * 2 / 5;
    polyline([cx - half, cx - half / 2, cx, cx + half / 2, cx + half, cx + half, cx - half],
             [y + h / 4, dip, y, dip, y + h / 4, y + h, y + h], 7, INK, null, true);
}

function pad7(n) {
    var s = "" + (n > 9999999 ? 9999999 : n);
    while (s.length < 7)
        s = "0" + s;
    return s;
}

// The band above the rule, at the screen's size: the score on the right,
// the crown and the multiplier beside it, the caption in what is left.
// Drawn only when one of them has changed, or after another screen: the
// rows are left as they are otherwise, which costs nothing to draw and less
// to send.
var bandPoints = -1, bandMult = 0, bandCrowned = false, bandStep = -1, bandText = "";
var lastFrame = -2;

var bandVolume = -1;

function bandChanged() {
    return points != bandPoints || mult != bandMult || crowned != bandCrowned ||
           caption.step != bandStep || caption.text != bandText || volume != bandVolume;
}

// The volume in the caption's room: a speaker, and a bar filled to it
function drawVolume(x, y, room) {
    var bx = x + 9, bw = room - 9;
    if (bw < 12)
        return;
    rectfill(x, y + 1, x + 2, y + CAP_H - 2, INK);
    trifill(x + 2, y + 1, x + 5, y - 1, x + 5, y + CAP_H, INK);
    trifill(x + 2, y + CAP_H - 2, x + 5, y + CAP_H, x + 2, y + 1, INK);
    rectfill(bx, y + 1, bx + bw - 1, y + CAP_H - 2, INK4);
    var fill = (bw - 2) * volume / 100;
    if (fill > 0)
        rectfill(bx + 1, y + 2, bx + fill, y + CAP_H - 3, INK);
}

function drawBand() {
    bandPoints = points;
    bandMult = mult;
    bandCrowned = crowned;
    bandStep = caption.step;
    bandText = caption.text;
    bandVolume = volume;
    camera();
    rectfill(0, 0, 159, HUD_H / 2, PAPER);
    font(FONT.CAPS);
    var s = pad7(points), sw = textw(s), th = texth("0");
    var y = (HUD_DROP + (BAND_H - th * 2) / 2) / 2;
    var x = 158 - sw;
    print(s, x, y, INK);
    // the crown left of the score, centred on its digits: the crown is
    // ch + 1 rows, the digits CAP_H
    var ch = th;
    x -= 3 + ch;
    if (crowned)
        crown(x, y - (ch + 1 - CAP_H) / 2, ch);
    x -= 3 + textw("X8");
    if (mult > 1)
        print("X" + mult, x, y, INK);
    if (volume >= 0)
        drawVolume(2, y, x - 8);
    else
        caption.draw(2, y, x - 8);
    line(0, HUD_H / 2, 159, HUD_H / 2, INK);
}

// What the field says over itself: paused, or waiting for the beat
function drawWords() {
    camera();
    font(FONT.CAPS);
    if (silent) {
        var w = "PAUSED";
        print(w, 80 - textw(w) / 2, HUD_H / 2 + 20, INK);
    } else if (waiting) {
        var a = "WAITING FOR THE", b = "BEAT";
        print(a, 80 - textw(a) / 2, HUD_H / 2 + 15, INK);
        print(b, 80 - textw(b) / 2, HUD_H / 2 + 28, phase < pose.PHASE / 2 ? ACCENT : INK);
    }
}

// ---- the frame ---------------------------------------------------------------

// The background: images/background.png, twice the screen's width, sitting
// on the ground line and going by a pixel a beat -- the course goes
// sixteen. It goes by on the music's clock rather than on the course's
// eased step, so its pixel is taken on the beat: by the ease, a few pixels
// bunch into each beat's start and read as a judder. Each of its rows costs about 70 units a frame. It is
// drawn opaque, its see-through sky in the paper colour, so the rows it
// covers need no clearing first.
const BACK_SPEED = 1;           // screen pixels a beat; the course goes 16
const BACK_W = 340, BACK_H = 48; // images/background.png's size: change with it
// ...and how far below the ground line its bottom row sits: its last rows
// are the ground's dither, behind the ground and seen only through a gap
const BACK_DROP = 3;

function drawBackground() {
    var w = BACK_W, h = BACK_H;
    var at = (nowMs * BACK_SPEED) / beatMs;
    var x = -(at % w), y = surfaceY(0) - h + 1 + BACK_DROP;
    palt(0, false);
    pal(0, PAPER);
    blit(IMG.background, x, y);
    if (x + w < 160)
        blit(IMG.background, x + w, y);
    pal(0);
    palt(0, true);
}

function draw() {
    var top = HUD_H / 2 + 1, back = surfaceY(0) - BACK_H + 1 + BACK_DROP;
    camera();
    // the band again where it has changed -- a frame late where this one
    // crossed a beat, which has work enough -- or where another screen drew
    if (frame() != lastFrame + 1 || (bandChanged() && !crossed))
        drawBand();
    lastFrame = frame();
    // the field's rows the background does not cover
    if (back > top)
        rectfill(0, top, 159, back - 1, PAPER);
    rectfill(0, surfaceY(0) + 1, 159, 119, PAPER);
    clip(0, top, 159, 119);
    drawBackground();
    drawWorld();
    camera(0, 0, 0, 0, 0, 0.5);
    trailStep();
    trailDraw();
    if (dropCells >= 0)
        drawDrop();
    else if (!skipping)
        drawPlayer();
    clip();
    drawWords();
}
