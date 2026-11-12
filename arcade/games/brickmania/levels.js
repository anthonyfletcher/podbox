// The ten levels. Each row is twelve letters, one a brick:
//
//   .  no brick
//   r o y g c v   a plain brick: red, orange, yellow, green, cyan, violet
//   S  strong: two hits, cracking at the first
//   A  armoured: three hits
//   M  metal: never breaks
//   X  explosive: breaks its neighbours, which may be explosive too
//   P  power brick: drops a power-up
//
// "moving" lists rows that slide from side to side: [row, how far either
// way in pixels, pixels a tick]. Leave room for that many pixels of empty
// columns at both ends of a moving row (12 pixels a column).
// "descend" is how many ticks the wall takes to creep down a pixel, 0 for
// a wall that stays put.

var all = [
    {   name: "FIRST WALL", moving: [], descend: 0,
        rows: ["............",
               "rrrrrrrrrrrr",
               "oooooooooooo",
               "yyPyyyyyyPyy",
               "gggggggggggg",
               "cccccccccccc"] },

    {   name: "CHECKERBOARD", moving: [], descend: 0,
        rows: ["r.r.r.r.r.r.",
               ".o.o.o.o.o.o",
               "y.y.y.P.y.y.",
               ".g.g.g.g.g.g",
               "c.c.P.c.c.c.",
               ".v.v.v.v.v.v"] },

    {   name: "DIAMOND", moving: [], descend: 0,
        rows: [".....SS.....",
               "....SrrS....",
               "...SoyyoS...",
               "..SogPPgoS..",
               "...SoyyoS...",
               "....SrrS....",
               ".....SS....."] },

    {   name: "CHANNELS", moving: [], descend: 0,
        rows: ["rrrrrrrrrrrr",
               "M.oo.MM.oo.M",
               "M.yy.MM.yy.M",
               "M.gg.MM.gg.M",
               "M.cP.MM.Pc.M",
               "M....MM....M"] },

    {   name: "ARMOURED KEEP", moving: [], descend: 0,
        rows: ["AAAAAAAAAAAA",
               "A..........A",
               "A.PyyyyyyP.A",
               "A.ySSSSSSy.A",
               "A.yyyyyyyy.A",
               "A..........A",
               "AAAAA..AAAAA"] },

    {   name: "MOVING ROWS", descend: 0,
        moving: [[1, 32, 0.5], [3, 40, -0.7], [5, 20, 0.35]],
        rows: ["............",
               "...rrrrrr...",
               "............",
               "....oPPo....",
               "............",
               "..yyyyyyyy..",
               "gSgSgSgSgSgS"] },

    {   name: "CHAIN REACTION", moving: [], descend: 0,
        rows: ["rrrrrrrrrrrr",
               "XoXoXoXoXoXo",
               "oXoXoXoXoXoX",
               "yyyyXXXXyyyy",
               "gggPggggPggg",
               "cccccccccccc"] },

    {   name: "HARD TARGETS", moving: [], descend: 0,
        rows: ["A....M....A.",
               "............",
               "...S....S...",
               "M....A....M.",
               "............",
               ".P..S..S..P.",
               "............",
               "A....M....A."] },

    {   name: "DESCENT", moving: [], descend: 90,
        rows: ["r..........r",
               "or........ro",
               "yor......roy",
               "gyor....royg",
               "cgyorPProygc",
               "vcgyorroygcv"] },

    {   name: "FINALE", descend: 0,
        moving: [[2, 20, 0.6], [4, 32, -0.5]],
        rows: ["MXXM....MXXM",
               "AAAAAAAAAAAA",
               "..rrrrrrrr..",
               "SPSPSPSPSPSP",
               "...oooooo...",
               "XyXyXyXyXyXy",
               "gggggggggggg"] }
];

// The six plain colours, in the order their bricks sit on the sheet
var PLAIN_LETTERS = "roygcv";

// Level n's wall built in bricks.js. After the tenth the levels come
// round again, the ball starting a little faster each time.
function load(n) {
    var l = all[(n - 1) % all.length];
    var lap = flr((n - 1) / all.length);
    bricks.clear();
    for (var r = 0; r < l.rows.length && r < ROWS; r++)
        for (var c = 0; c < COLS; c++)
            place(r, c, l.rows[r][c]);
    for (var m = 0; m < l.moving.length; m++)
        bricks.set_motion(l.moving[m][0], l.moving[m][1], l.moving[m][2]);
    bricks.set_descent(l.descend);
    balls.set_start_speed(lap);
}

function name(n) {
    return all[(n - 1) % all.length].name;
}

// One letter of a level as a brick
function place(r, c, letter) {
    var colour = PLAIN_LETTERS.indexOf(letter);
    if (colour >= 0) bricks.place(r, c, bricks.K_PLAIN, colour);
    else if (letter == "S") bricks.place(r, c, bricks.K_STRONG, 0);
    else if (letter == "A") bricks.place(r, c, bricks.K_ARMOUR, 0);
    else if (letter == "M") bricks.place(r, c, bricks.K_METAL, 0);
    else if (letter == "X") bricks.place(r, c, bricks.K_BOMB, 0);
    else if (letter == "P") bricks.place(r, c, bricks.K_POWER, 0);
}
