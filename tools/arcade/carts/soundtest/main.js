// Sound Test: the cart's effects, tones and the wheel's click.
// The wheel picks an effect; SELECT plays it, PLAY an octave up.
// Hold PREV or NEXT for a rising or falling tone. Each step of the wheel
// clicks the iPod's own clicker, and passing either end of the list beeps
// it: listen at the iPod itself, not the headphones.

var names = ["blip", "coin", "jump", "laser", "boom", "powerup"];
var sounds = [SFX.a_blip, SFX.b_coin, SFX.c_jump, SFX.d_laser, SFX.e_boom,
              SFX.f_powerup];
var pick = 0, turn = 0, pitch = 220, played = "";

function init() {}

function update() {
    turn += wheel();
    if (turn >= 24 || turn <= -24) {
        var step = turn > 0 ? 1 : names.length - 1;
        turn = 0;
        var was = pick;
        pick = (pick + step) % names.length;
        click(abs(pick - was) > 1);
    }
    if (btnp(4)) { sfx(sounds[pick]); played = names[pick]; }
    if (btnp(3)) { sfxp(sounds[pick], 12); played = names[pick] + " +12"; }
    if (btn(0)) { pitch = min(pitch + 8, 1760); tone(3, pitch, 2, 160, WAVE.TRIANGLE); }
    if (btn(1)) { pitch = max(pitch - 8, 110); tone(3, pitch, 2, 160, WAVE.SQUARE); }
}

function draw() {
    cls(1);
    print("SOUND TEST", 4, 4, 7);
    for (var i = 0; i < names.length; i++) {
        if (i == pick) rectfill(2, 16 + i * 9, 70, 24 + i * 9, 12);
        print(names[i], 6, 18 + i * 9, i == pick ? 0 : 6);
    }
    print("played: " + played, 80, 18, 10);
    print("tone: " + pitch + " Hz", 80, 28, 10);
    print("wheel picks", 4, 84, 13);
    print("SELECT plays, PLAY an octave up", 4, 92, 13);
    print("PREV/NEXT held: tones up/down", 4, 100, 13);
    print("the wheel clicks; the ends beep", 4, 108, 13);
}
