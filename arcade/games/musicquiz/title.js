// title: the title screen -- the title in the middle, and notes, stars and
// pluses sprayed out from behind it, faster as they go.

const TITLE_W = 93, TITLE_H = 77, TITLE_X = 33, TITLE_Y = 14;
const CX = 80, CY = TITLE_Y + 40;     // where the spray comes from
const N = 48;                         // the most in flight at once
const EVERY = 2;                      // ticks between one and the next

// The sprites: three notes and three stars, 2x2; four pluses, 1x1
var kinds = [0, 2, 4, 32, 34, 36, 64, 65, 66, 67, 64, 65, 66, 67];

var px = [], py = [], vx = [], vy = [], sp = [], size = [], live = [];

function start() {
    if (px.length == 0)
        for (var i = 0; i < N; i++) {
            px.push(0.0); py.push(0.0); vx.push(0.0); vy.push(0.0);
            sp.push(0); size.push(1); live.push(false);
        }
    for (var j = 0; j < N; j++) live[j] = false;
    // already in full flow when it is first seen
    for (var t = 0; t < 90; t++) step(t);
}

function spawn() {
    for (var i = 0; i < N; i++) {
        if (live[i]) continue;
        var a = rnd(1.0), speed = 0.4 + rnd(0.8);
        sp[i] = rnd(kinds);
        size[i] = sp[i] < 64 ? 2 : 1;
        px[i] = CX - size[i] * 4;
        py[i] = CY - size[i] * 4;
        vx[i] = cos(a) * speed;
        vy[i] = sin(a) * speed;
        live[i] = true;
        return;
    }
}

function step(t) {
    if (t % EVERY == 0) spawn();
    for (var i = 0; i < N; i++) {
        if (!live[i]) continue;
        px[i] += vx[i];
        py[i] += vy[i];
        vx[i] *= 1.03;
        vy[i] *= 1.03;
        if (px[i] < -16 || px[i] > 160 || py[i] < -16 || py[i] > 120)
            live[i] = false;
    }
}

function update() {
    step(frame());
}

// The spray behind the title, then the title
function draw() {
    for (var i = 0; i < N; i++)
        if (live[i]) spr(sp[i], px[i], py[i], size[i], size[i]);
    blit(IMG.title, TITLE_X, TITLE_Y);
}
