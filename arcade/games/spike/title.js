// The title, while there is no music to play to: the logo, Spike messing
// about on the ground -- the results screen's routine of somersaults and
// balances, a move every MOVE_MS of wall time -- and what to do to start.

const MOVE_MS = 800;
const TITLE_W = 98;             // images/title.png's width: change with it

var move = -1, moveAt = 0;
var body = pose.newPose();
var px = [0, 0, 0], py = [0, 0, 0];

function update() {
    if (move < 0 || flr((t() - moveAt) * 1000) >= MOVE_MS) {
        move = pose.idleNext(move);
        moveAt = t();
    }
}

function draw() {
    var g = field.surfaceY(0);
    cls(field.PAPER);
    camera();
    blit(IMG.background, 0, g - field.BACK_H + 1 + field.BACK_DROP);
    for (var x = 0; x < 160; x += 16)
        spr(field.S_GROUND, x, g, 2, 1);
    blit(IMG.title, 80 - TITLE_W / 2, 6);
    font(FONT.CAPS);
    var a = "PLAY SOME MUSIC", b = "TO START A RUN";
    print(a, 80 - textw(a) / 2, 54, field.INK);
    print(b, 80 - textw(b) / 2, 63, field.INK);

    var phase = flr((t() - moveAt) * 1000) * pose.PHASE / MOVE_MS;
    if (phase > pose.PHASE - 1)
        phase = pose.PHASE - 1;
    pose.idle(body, phase < 0 ? 0 : phase, move < 0 ? 0 : move);
    camera(0, 0, 0, 0, 0, 0.5);
    pose.points(body, 160, field.GROUND_Y, px, py);
    line(px[0], py[0], px[1], py[1], field.INK);
    line(px[1], py[1], px[2], py[2], field.INK);
    line(px[2], py[2], px[0], py[0], field.INK);
    camera();
}
