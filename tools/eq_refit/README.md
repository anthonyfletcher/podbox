# EQ Refit

## Re-fit a Rockbox EQ preset onto fewer bands, preserving its response.

Every enabled EQ band costs the DSP a full biquad pass over every sample of
every channel: five 64-bit multiply-accumulates per sample, per band, per
channel (lib/rbcodec/dsp/dsp_arm.S, filter_process). On PP5022 at 80 MHz that
is roughly 5% of the CPU per active band at 44.1 kHz stereo, so a preset using
nine bands spends about 45% of the machine on the equaliser alone and starves
the UI.

Bands whose gain is exactly zero are skipped entirely (lib/rbcodec/dsp/eq.c,
dsp_set_eq_coefs), so the cost already scales with bands actually used. This
script takes an existing preset, measures the frequency response it produces,
and searches for the smallest set of bands that reproduces that response to
within a stated tolerance in dB. Broad, smooth curves -- which is what all the
shipped presets are -- fit comfortably in three or four bands.

The filter math below mirrors lib/rbcodec/dsp/dsp_filter.c, which implements
the Robert Bristow-Johnson Audio EQ Cookbook formulas in fixed point:

```
filter_pk_coefs  -> cookbook peakingEQ   (EQ bands 1..8)
filter_ls_coefs  -> cookbook lowShelf    (EQ band 0)
filter_hs_coefs  -> cookbook highShelf   (EQ band 9)
```

with A = 10^(dB/40) and alpha = sin(w0)/(2*Q). Both scalings were checked
against the fixed-point source: get_replaygain_int(db*5) is 10^(dB/40) for a
gain held in tenths of a dB, and fp_sincos returns s0.31, which makes
`fp_sincos(cutoff, &cs)/(2*Q)*10 >> 1` equal sin(w0)/(2*Q) once Q's own
factor-of-ten storage is divided out. 

Note that filter_shelf_coefs / filter_bishelf_coefs in the same file are 
*first* order and belong to the bass/treble tone controls, not to the equaliser.

Responses are frequency-warped by the sample rate, so a fit is only exact at
the rate it was fitted for; 44.1 kHz is the default and covers most material.

Usage:
```
py tools/eq_refit.py eqs/Default.cfg
py tools/eq_refit.py eqs/*.cfg --out-dir eqs-refit
py tools/eq_refit.py eqs/Warm.cfg --bands 3 --quiet
```