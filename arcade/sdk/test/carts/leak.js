// random ragged ground (a closed outline); a bag dropped on it; gravity
// tilting; every tick each point is tested inside the ground polygon
var bag, N = 16, gx = [], gy = [], leaks = 0, out = [];
function inside(x, y) {
    var c = 0, n = gx.length;
    for (var i = 0, j = n - 1; i < n; j = i++) {
        if ((gy[i] > y) != (gy[j] > y) &&
            x < (gx[j] - gx[i]) * (y - gy[i]) / (gy[j] - gy[i]) + gx[i]) c = 1 - c;
    }
    return c;
}
function init() {
    var k = 6 + rnd(14);
    for (var i = 0; i <= k; i++) { gx.push(i * 160 / k); gy.push(60 + rnd(50)); }
    gx.push(160); gy.push(119); gx.push(0); gy.push(119);
    bag = links.pool(N, N);
    var cx = 20 + rnd(120);
    for (var q = 0; q < N; q++) { links.point(bag, cx + cos(q / 16.0) * 8, 15 + sin(q / 16.0) * 8); out.push(0, 0, 0); }
    for (var l = 0; l < N; l++) links.link(bag, l, (l + 1) % N);
    bag.gravity = 0.1 + rnd(30) * 0.01; bag.damp = rnd(5) * 0.2; bag.slack = 1; bag.passes = 4 + rnd(5);
    bag.radius = 0.5 + rnd(4) * 0.5; bag.friction = rnd(5) * 0.25; bag.bounce = rnd(3) * 0.2;
    bag.segs = segs.make(gx, gy, gx.length);
}
function update() {
    bag.gx = sin(frame() * 0.01) * 0.2;
    links.ring(bag, 0, N, 150, 0.4);
    links.step(bag, 0, out);
    for (var i = 0; i < N; i++) if (inside(bag.x[i], bag.y[i])) leaks++;
    if (frame() == 200) printh("leaks " + leaks);
}
function draw() {}
