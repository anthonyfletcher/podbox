# Sound

Sound effects: the coin, the laser, the explosion. A cart makes them in
the IDE's sound editor and plays them over the owner's music, which keeps
playing all the while -- there is no music player of a cart's own (see
**The owner's music** for what a cart can do with theirs).

- [How sound works](#how-sound-works)
- [Making effects in the IDE](#making-effects-in-the-ide), and [the file](#the-effect-file)
- [sfx](#sfxn-channel), [sfxat](#sfxatchannel), [sfxp](#sfxpn-semitones-channel), [tone](#tonechannel-hz-ticks-vol-wave)
- The wheel's click is in **Input**: [click](reference-input.md#clickbeep)

## How sound works

**Four channels** play at once, numbered 0 to 3. Each plays one sound at a
time -- an effect, or a plain tone -- and starting a sound on a channel
stops whatever it was playing. Leave the channel out and the Arcade picks
one: the first that is silent, or, if all four are busy, the one nearest
its end.

**Effects are made when the cart is built.** An effect is a recipe of
[sfxr](https://www.drpetter.se/project_sfxr.html) settings -- the generator
behind thousands of small games' sounds -- and the build turns it into
sound and stores it in the cart, at 22,050 samples a second, a byte each.
So:

- an effect sounds the same every time, and on every iPod;
- it costs memory: a second of sound is about 21.5 KB of the cart's 512 KB
  -- most effects are a tenth of a second or so, a couple of KB;
- playing one costs almost nothing.

**Where it is heard.** Effects go to the headphones or dock, mixed over
the owner's music, at the iPod's volume. (The wheel's click, `click()`,
is the separate little speaker inside the iPod.) Each effect has its own
loudness, set when it is made; the owner's volume is the music's, which a
cart with `"music": "control"` can turn with
[`playback.adjust`](reference-music.md#controlling-it).

**Game Sound.** The pause menu's **Game Sound** row mutes a game's own
effects and tones -- not the owner's music, not speech -- and the iPod
remembers it for that game. A muted game plays on as before: each effect
still lasts as long, and `sfxat` still reports it; only silence is heard.
Every cart has the row, so a cart needs no sound option of its own.

**What it costs**: about 11 units to start an effect, 14 a tone. The
mixing is done by the iPod, not counted against the cart.

---

## Making effects in the IDE

Choose **File > New file**, then **A sound**, and name it. It opens in the
sound editor:

- **Generate**, the panel beside it, makes a new effect of a kind, as
  sfxr's own buttons do: **coin**, **laser**, **explosion**, **power-up**,
  **hit**, **jump** and **blip**, each a little different every time;
  **random** for anything at all; and **mutate** to vary the one you have
  a little. Press one until you like what you hear.
- **The toolbar**: play it (or press **Space**); the four waves it is made
  from -- square, saw, sine and noise; and its length and size.
- **The sliders**, in sections: the envelope (how it rises, holds and
  fades), the pitch and its slide, vibrato, a jump in pitch partway
  (arpeggio), the square wave's shape, repeating, a flanger, and two
  filters. A slider plays the sound when you let it go; double-click one
  to put it back.
- **The picture** is the sound as the cart will store it -- click it to
  play.
- **Ctrl+Z** undoes.

What the editor plays is exactly what the iPod plays: the same 8-bit
sound at 22,050 a second.

Each effect is `SFX.<name>` in code, after its file name.

## The effect file

An effect is saved as `sounds/<name>.json`, sfxr's settings by the names
jsfxr -- the web version of sfxr -- uses:

```json
{
  "wave_type": 0,
  "p_env_attack": 0,
  "p_env_sustain": 0.022,
  "p_env_punch": 0.366,
  "p_env_decay": 0.188,
  "p_base_freq": 0.513,
  "sound_vol": 0.5
}
```

Any setting left out takes sfxr's default. So an effect made on jsfxr's
web page can be saved straight into `sounds/`. `wave_type` is 0 square,
1 saw, 2 sine, 3 noise; `sound_vol` its loudness, 0 to 1.

---

## sfx(n, [channel])

Plays an effect.

| Argument | |
|---|---|
| `n` | the effect: `SFX.<name>`; or -1 to stop a channel |
| `channel` | the channel, 0 to 3; left out, the first silent one, else the one nearest its end |

**Returns** nothing.

```js
function update() {
    if (btnp(4)) sfx(SFX.laser);              // a shot on Select
    if (hitCoin()) sfx(SFX.coin, 1);          // coins always on channel 1
}
```

- It starts at once and plays to its end while the game carries on; the
  cart does nothing more.
- **Giving a channel** keeps a sound from cutting others off -- or makes
  it cut itself off: coins on channel 1 means a run of quick pickups
  restarts the coin each time rather than piling four up. Leave music-like
  sounds their own channel, and let the rest share.
- `sfx(-1, channel)` stops that channel. Always give the channel with -1:
  left out, it stops whichever channel a new sound would have taken.
- A number that is not one of the cart's effects, or a channel other than
  0 to 3, stops the cart with an error.

## sfxat(channel)

What a channel is playing.

| Argument | |
|---|---|
| `channel` | 0 to 3 |

**Returns** the effect's number -- the same as `SFX.<name>` -- or -2 for a
`tone`, or -1 when the channel is silent.

```js
// a jingle that waits for the last one to end
if (sfxat(3) == -1) sfx(SFX.coin, 3);
```

- The sound is mixed a little ahead of the speaker, so a channel can
  report silence a few hundredths of a second before its sound has been
  heard to the end.

## sfxp(n, semitones, [channel])

Plays an effect higher or lower.

| Argument | |
|---|---|
| `n` | the effect: `SFX.<name>` |
| `semitones` | how far to move it: 12 an octave up, -12 an octave down; -48 to 48 |
| `channel` | as for `sfx` |

**Returns** nothing.

```js
// each coin in a row a step higher, as in so many platformers
sfxp(SFX.coin, min(streak, 12), 1);
streak++;
```

- It plays faster as it goes higher, as a record does: an octave up
  plays in half the time, an octave down in twice.
- One effect played at several pitches makes a little tune, or a sound
  that rises with the score, without storing several.

## tone(channel, hz, ticks, vol, wave)

Plays a plain note, made as it plays rather than stored.

| Argument | |
|---|---|
| `channel` | the channel, 0 to 3, or -1 for the first silent one |
| `hz` | its pitch, in hertz: 440 is the A above middle C; up to 20,000 |
| `ticks` | how long, in ticks of the cart (at 33 a second, 33 is a second); at most 3,000 |
| `vol` | its loudness, 0 to 255 |
| `wave` | `WAVE.SQUARE`, `WAVE.SAW`, `WAVE.TRIANGLE`, `WAVE.SINE` or `WAVE.NOISE` |

**Returns** nothing.

```js
// a beep that rises with the countdown: 3, 2, 1, go
var NOTES = [440, 440, 440, 880];

function countdown(i) {
    tone(0, NOTES[i], i == 3 ? 20 : 6, 160, WAVE.SQUARE);
}
```

- The waves, from bright and buzzy to soft: square, saw, triangle,
  sine. Noise is a hiss, its pitch how coarse it is -- for wind, engines
  and static.
- Its length follows the cart's rate, so `tone(0, 440, 33, ...)` lasts a
  second at 33 ticks a second and longer if the cart steps down to 25.
  Ten ticks at 33 is 303 ms.
- A pitch, length or loudness of 0 or less silences the channel instead.
  A wave other than the five stops the cart with an error.
- Tones need no file and no memory, so they suit sounds the cart works
  out as it goes: a pitch from the music's beat, a note per row cleared,
  the beeps of a timer.
- A tone at 255 is about half as loud again as an effect at its default
  `sound_vol` of 0.5; 160 or so sits level with effects.
