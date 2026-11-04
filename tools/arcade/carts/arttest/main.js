// Art Test: the cart's own sheet, palettes, maps, objects and image.
// SELECT swaps the map, PLAY the palette; the wheel scrolls the strip.

var level = 0, night = 0, scroll = 0;
var objs = new Int32Array(40);
var count = 0;

function init() {
    use(MAP.field);
}

function use(m) {
    level = m;
    loadmap(m);
    scroll = 0;
    count = mapobj(objs, 8);
}

function update() {
    if (btnp(4)) use(level == MAP.field ? MAP.strip : MAP.field);
    if (btnp(3)) {
        night = 1 - night;
        loadpal(night ? PAL.b_night : PAL.a_day);
    }
    scroll = mid(0, scroll + wheel(), (mapw() - 20) * 8);
}

function draw() {
    cls(0);
    camera(scroll, 0);
    map(0, 0, 0, 0, mapw(), maph());
    for (var i = 0; i < count; i++) {
        var k = objs[i * 5], x = objs[i * 5 + 1], y = objs[i * 5 + 2];
        if (k == OBJ.coin) spr(5, x, y);
        else spr(6, x, y, 1, 2);
    }
    camera();
    blit(IMG.title, 4, 4);
    print(level == MAP.field ? "FIELD" : "STRIP", 10, 10, 7);
    print(mapw() + "x" + maph() + " " + count + " objs", 10, 17, 7);
    for (var j = 0; j < count; j++) {
        var v = mapprop(j, "value"), to = mapprop(j, "to");
        print((objs[j * 5] == OBJ.coin ? "coin " + v : "door to " + to), 100, 4 + j * 7, 7);
    }
    print("SELECT map  PLAY palette  wheel scrolls", 2, 113, 6);
}
