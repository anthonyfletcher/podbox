# pfgeom — checking the carousel's occlusion cull

The album-covers carousel does not draw the parts of a cover that a nearer one
already hides (`cull_side()` and `slide_covers()` in
`apps-ipod/screens/covers/carousel.c`). That is worth about a quarter of the
pixels in a scrolling frame, and the cost of getting it wrong is a stripe of
stale framebuffer that nothing later repaints — on one album shape, at one
point in one scroll, on hardware.

`pfgeom` decides the question on the host instead. It mirrors the carousel's
projection, cull and draw order, renders each frame twice — once with the cull
and once without — and compares **which slide finished on top of every pixel**.
That is the exact test: the topmost writer is what picks the colour, so if the
two renders agree everywhere then the cull removed only pixels that were going
to be painted over anyway.

It sweeps the settings that move the geometry (centre margin, slide tuck,
parallel slides), the two things that resize the viewport (status bar height and
caption band), the scroll in both directions, and — the case that catches real
bugs — covers of different shapes, since art keeps its aspect ratio and a tall
or wide cover does not decode to the same size as a square one.

Nothing here is built or shipped. It has no Rockbox headers and no dependencies.

```sh
gcc -O2 -W -Wall -Wextra -std=gnu11 -o pfgeom tools/pfgeom/pfgeom.c
./pfgeom          # the sweep; exit status is 0 only if nothing went stale
./pfgeom -x       # compare dy at every column instead of sampling three
./pfgeom -v       # print every frame, not just the failures
```

`-x` is the one worth knowing about. `slide_covers()` decides whether one slide
hides another by comparing how compressed each is at three columns — the two
ends of the overlap and the middle — rather than at all of them, because it runs
per frame on the device. `-x` compares every column, so running both says
whether the sampling ever gets a different answer from the exhaustive check. It
does not, across the whole sweep.

**Update this file's mirror when `carousel.c`'s geometry changes.** The banner
in `pfgeom.c` marks the copied region: the fixed-point helpers, `fsin`,
`recalc_offsets`, `coverflow_idle`, `coverflow_animate`, `slide_x_range`,
`slide_dy_at`, `slide_covers`, `cull_side`, and the column walk and draw order
out of `render_slide_clipped`/`coverflow_render`. A mirror that has drifted
still passes — it is checking itself — so the answer it gives is only worth what
the copy is.
