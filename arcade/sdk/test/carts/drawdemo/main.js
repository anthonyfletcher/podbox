var buf;

function init() {
    // Two sprites drawn into the sheet: a smiley (1) and a brick (2).
    var face = ["..aaaa..", ".aaaaaa.", "aa0aa0aa", "aaaaaaaa",
                "a0aaaa0a", "aa0000aa", ".aaaaaa.", "..aaaa.."];
    var x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            var ch = face[y].charCodeAt(x);
            sset(8 + x, y, ch == 97 ? 10 : ch == 48 ? 1 : 0);
            sset(16 + x, y, y == 0 || x == 0 ? 15 : (y == 7 || x == 7 ? 2 : 8));
        }
    fset(2, 1, 0);
    for (x = 0; x < 16; x++) mset(x, 0, x % 2 ? 2 : 1);
    buf = img(24, 16);
    target(buf);
    cls(1);
    circfill(12, 8, 6, 14);
    print("img", 7, 6, 7);
    target(SCREEN);
}

function update() {}

function draw() {
    cls(1);
    rectfill(0, 0, 127, 7, 5);
    print("ARCADE draw test 0123", 2, 1, 7);
    rect(2, 10, 21, 25, 8);
    rectfill(25, 10, 44, 25, 9);
    circ(57, 18, 7, 10);
    circfill(76, 18, 7, 11);
    oval(88, 11, 107, 25, 12);
    ovalfill(110, 11, 125, 25, 14);
    line(2, 30, 60, 44, 7);
    line(2, 44, 60, 30, 6);
    tri(66, 44, 80, 29, 94, 44, 15);
    trifill(98, 44, 112, 29, 126, 44, 13);
    fillp(0xa5a5);
    rectfill(2, 48, 30, 62, 0x0c07);
    circfill(46, 55, 7, 8);
    fillp();
    spr(1, 60, 48);
    spr(1, 70, 48, 1, 1, true);
    sspr(8, 0, 8, 8, 80, 48, 16, 16);
    map(0, 0, 98, 48, 3, 1);
    map(0, 0, 98, 56, 4, 1, 1);
    blit(buf, 2, 66);
    blitpart(buf, 6, 2, 12, 12, 30, 68, true);
    pal(8, 12);
    rectfill(48, 66, 60, 78, 8);
    pal();
    camera(-64, -70);
    circ(0, 0, 4, 7);
    camera();
    clip(80, 66, 99, 77);
    circfill(90, 72, 12, 9);
    clip();
    tline(102, 70, 126, 70, 0, 0, 0.125, 0);
    tline(102, 74, 126, 74, 0, 0, 0.0625, 0);
    font(FONT.CAPS, 2);
    print("BIG", 2, 84, 10);
    font(FONT.CAPS);
    var w = textw("width test");
    print("width test", 40, 86, 7);
    line(40, 92, 40 + w - 2, 92, 8);
    for (var i = 0; i < 16; i++) rectfill(2 + i * 7, 100, 7 + i * 7, 106, i);
    cursor(2, 110, 6);
    print("cursor line one");
    print("and the next");
    // The screen is 160 wide: a side panel in the last 32 columns.
    rectfill(128, 0, 159, 119, 2);
    print("SIDE", 136, 4, 7);
    rectfill(132, 112, 139, 117, 11);
}
