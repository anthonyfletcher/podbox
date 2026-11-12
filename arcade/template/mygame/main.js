// My Game: a ball the wheel steers round the screen. Turn the wheel to
// move it; press Select to change its colour.

var x = 80, y = 60;     // the ball, in pixels: the screen is 160 by 120
var angle = 0.0;        // which way it is heading, in turns
var c = 10;             // its colour, from the palette

function init() {
}

function update() {
    angle += wheel() / 96.0;          // a whole turn of the wheel, a whole turn
    x = mid(4, x + cos(angle) * 1.5, 155);
    y = mid(4, y + sin(angle) * 1.5, 115);
    if (btnp(4)) c = c % 15 + 1;      // Select: the next colour
}

function draw() {
    cls(1);
    circfill(x, y, 3, c);
    print("TURN THE WHEEL", 2, 2, 7);
}
