// title: the title screen's logo, with balls bouncing round the playfield
// behind it.

const LOGO_W = 92, LOGO_H = 49;
const LOGO_X = 80 - LOGO_W / 2, LOGO_Y = 18;
const N = 6;                    // balls bouncing

var bx = [], by = [], bdx = [], bdy = [], kind = [];

function setup() {
    for (var i = 0; i < N; i++) {
        var a = rnd(1.0), speed = 0.8 + rnd(0.6);
        bx.push(FIELD_L + 2 + rnd(140.0));
        by.push(FIELD_T + 2 + rnd(100.0));
        bdx.push(cos(a) * speed);
        bdy.push(sin(a) * speed);
        kind.push(i % 4);       // normal, fire, sticky, slow in turn
    }
}

// Each ball on a step, turned back by the walls -- and by the floor, which
// on the title is a wall too
function update() {
    for (var i = 0; i < N; i++) {
        bx[i] += bdx[i];
        by[i] += bdy[i];
        if (bx[i] - 2 < FIELD_L || bx[i] + 1 >= FIELD_R) {
            bdx[i] = -bdx[i];
            bx[i] = mid(FIELD_L + 2, bx[i], FIELD_R - 2);
        }
        if (by[i] - 2 < FIELD_T || by[i] + 1 >= FIELD_B) {
            bdy[i] = -bdy[i];
            by[i] = mid(FIELD_T + 2, by[i], FIELD_B - 2);
        }
    }
}

function draw() {
    for (var i = 0; i < N; i++)
        sspr(kind[i] * 8, SHEET_BALL_Y, 4, 4, bx[i] - 2, by[i] - 2);
    blit(IMG.title, LOGO_X, LOGO_Y);
}
