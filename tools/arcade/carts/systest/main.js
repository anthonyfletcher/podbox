// Systems Test: actors bouncing in a walled arena under gravity, off the
// walls and each other, and particles spraying from a fountain (SELECT)
// and bursting where two actors meet.

var balls, sparks, contacts = new Int32Array(400), pairs = new Int32Array(200);
var hits = 0, met = 0;

function init() {
    // sprite 1: a wall block (flag 1); sprite 2: a ball
    for (var y = 0; y < 8; y++)
        for (var x = 0; x < 8; x++) {
            sset(8 + x, y, x == 0 || y == 0 ? 6 : x == 7 || y == 7 ? 5 : 13);
            var d = (x - 3.5) * (x - 3.5) + (y - 3.5) * (y - 3.5);
            sset(16 + x, y, d < 6 ? 12 : d < 12 ? 7 : 0);
        }
    fset(1, 1);
    for (var c = 0; c < 20; c++) { mset(c, 0, 1); mset(c, 14, 1); }
    for (var r = 0; r < 15; r++) { mset(0, r, 1); mset(19, r, 1); }
    for (c = 6; c < 14; c++) mset(c, 9, 1);
    balls = actors.pool(24);
    balls.gravity = 0.15;
    for (var i = 0; i < 24; i++) {
        var a = actors.spawn(balls);
        balls.x[a] = 12 + (i % 12) * 11;
        balls.y[a] = 12 + flr(i / 12) * 14;
        balls.dx[a] = rnd(3.0) - 1.5;
        balls.sp[a] = 2;
    }
    sparks = particles.pool(300);
    sparks.gravity = 0.08;
}

function update() {
    hits = actors.move(balls, 1, 0, contacts);
    for (var k = 0; k < hits; k++) {
        var i = contacts[k * 4], side = contacts[k * 4 + 1];
        if (side == HIT.LEFT || side == HIT.RIGHT) balls.dx[i] = -balls.dx[i] * 0.9;
        else balls.dy[i] = side == HIT.DOWN ? -3.2 - rnd(1.0) : -balls.dy[i];
    }
    met = actors.overlaps(balls, balls, pairs);
    for (k = 0; k < met; k++) {
        var p = pairs[k * 2], q = pairs[k * 2 + 1];
        // apart, each away from the other; sparks only when they close
        var closing = (balls.x[p] < balls.x[q]) == (balls.dx[p] > balls.dx[q]);
        if (balls.x[p] < balls.x[q]) {
            balls.dx[p] = -abs(balls.dx[p]) - 0.2;
            balls.dx[q] = abs(balls.dx[q]) + 0.2;
        } else {
            balls.dx[p] = abs(balls.dx[p]) + 0.2;
            balls.dx[q] = -abs(balls.dx[q]) - 0.2;
        }
        if (closing)
            particles.emit(sparks, 6, balls.x[p] + 4, balls.y[p] + 4, 0, -0.5, 1.2, 14, 10);
    }
    if (btn(4)) particles.emit(sparks, 8, 80, 70, 0, -2.5, 1.0, 40, 12 + flr(rnd(3)));
    particles.step(sparks);
}

function draw() {
    cls(1);
    map(0, 0, 0, 0, 20, 15);
    actors.draw(balls);
    particles.draw(sparks);
    var live = 0;
    for (var i = 0; i < 300; i++) if (sparks.life[i] > 0) live++;
    print("hits " + hits + "  pairs " + met + "  sparks " + live, 10, 2, 7);
    print("SELECT: fountain", 10, 113, 6);
}
