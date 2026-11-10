# The Arcade manual

For anyone who has written a little code and wants to make a game for
the iPod's Arcade.

Start with the tutorial: six chapters that build one game, a bat-and-ball
game, from an empty cart to something on your iPod. Then use the
reference for whatever your own game needs.

## Learning, a chapter at a time

1. [A ball and a bat](tutorial-1-ball-and-bat.md) -- the screen, colours,
   the three functions that run a game, moving things, the wheel
2. [Score, lives and game over](tutorial-2-score-and-lives.md) -- text,
   functions of your own, loops, and the game's state
3. [Sprites](tutorial-3-sprites.md) -- drawing the ball and bat on the
   sprite sheet, and animating them
4. [Sound](tutorial-4-sound.md) -- sound effects for hits and misses
5. [High scores and the pause menu](tutorial-5-high-scores.md) -- a board
   the player keeps, an option, and the pause menu
6. [On your iPod](tutorial-6-on-the-ipod.md) -- putting it on the iPod,
   making sure it keeps up there, and sharing it as a web page

## Reference

Each chapter explains how its part of the Arcade works, then every
function in it: its arguments, what it returns, what to watch for, an
example and what it costs.

**The language**

- [The language](reference-language.md) -- numbers, strings, arrays,
  records, functions, the maths functions, and what JavaScript has that
  the Arcade does not

**Drawing**

- [Drawing](reference-drawing.md) -- shapes, pixels, and the drawing
  state: pen colour, fill pattern, camera, clip, target
- [Text and fonts](reference-text.md) -- print, measuring text, the
  built-in fonts and fonts of your own
- [Colour and palettes](reference-colour.md) -- the palette, swapping and
  changing colours, transparency, fades
- [Sprites](reference-sprites.md) -- the sprite sheet, drawing, turning
  and flagging sprites, and sheets of your own
- [Images](reference-images.md) -- pictures in the cart, images made in
  code, and drawing into them
- [Maps and objects](reference-maps.md) -- tile maps, drawing and reading
  them, objects, and collision with the map

**Playing**

- [Input and the wheel](reference-input.md) -- the buttons, the wheel and
  its libraries
- [Sound](reference-sound.md) -- sound effects and tones
- [Time and speed](reference-time.md) -- ticks, the rate, late frames and
  the date

**The player's services**

- [Saving](reference-saving.md) -- saved numbers, stored values and data
  files
- [Scores, options and the pause menu](reference-scores.md) --
  high-score boards, options, achievements and the pause menu
- [Dialogs and speech](reference-dialogs.md) -- the player's own dialogs,
  speaking, and the owner's settings
- [The owner's music and the beat](reference-music.md) -- what is playing,
  controlling it, the library, and playing to the beat

**Many things at once**

- [Moving many things](reference-actors.md) -- actors, particles and
  sprite batches
- [Boards and paths](reference-boards.md) -- grids for puzzles, finding
  paths, and noise
- [Simulations](reference-simulations.md) -- falling sand, Life, ropes
  and cloth
- [3D views](reference-3d.md) -- a map as a floor in perspective,
  landscapes, walls and light

**Making it fast**

- [Making it fast](reference-fast.md) -- what the iPod can do in a tick,
  what everything costs, the profiler and replays
