# Featured Artists

Your library already knows who guested on what. It is sitting in the track
titles — *Song (feat. Somebody)* — and sometimes in a track's artist, where a
compilation rip leaves *Somebody feat. Someone Else*. 

**Featured Artists** reads that prose instead, and turns it into something you
can browse. It is off until you turn it on: **Settings > Library > Music >
Featured Artists**. It also requires that the database is loaded to RAM.

---

## Where it appears

**Music > Featured Artists** lists every guest the library credits, by name,
with the number of tracks each appears on — *Somebody (5)*. The list item 
only appears if there are Featured Artists to display.

If a guest also has albums of their own on the player, holding **Select** on
their row goes to those instead.

**[Featured In]**, on an artist's own album list, sits under `[All Tracks]` and 
`[Random]`, and only appears if an artist has featured on someone else's track.

---

## What counts as a credit

The words it looks for are **feat.**, **ft.**, **featuring** and **w/**, in
any capitalisation. 

Where the credit ends depends on the brackets:

| In your tag | Guest |
|---|---|
| `Song (feat. Ann Vey)` | Ann Vey |
| `Song (feat. Ann Vey) [Remastered]` | Ann Vey |
| `Song feat. Ann Vey (Live)` | Ann Vey |

A bracket ends a credit whether it opens or closes one, which is what keeps
*(Live)* and *[Remastered]* out of somebody's name.

Only the first credit in a tag is read. A title crediting two people in two
separate brackets gives you the first.

---

## Where one name ends and the next begins

Several guests in one credit are separated by `&`, `,`, `;`, `+`, `/`, the
word *and*, or a lone *x*. So `feat. Ann Vey & Kites` is two people.

Which is fine until a name has that punctuation *inside* it. *Earth, Wind & 
Fire* is one artist, and nothing in the tag says whether that comma and is
inside a name or between two.

The one thing the player can check is your library. If it has albums filed
under *Nick Cave & the Bad Seeds*, it knows to keep that name whole, and it is
clever enough to apply that inside a longer credit — *feat. Nick Cave & the
Bad Seeds & Kites* gives you two guests, not three.

That works for the artists you have tracks by. Guests, by their nature, are
often not among them.

---

## Names the player would otherwise split

`/.rockbox/known_artists.txt` is how you fill that gap: a list of names you are
vouching for. Make a plain text file with one name per line.

```
# Names that would otherwise be read as two people.

Earth, Wind & Fire
Blood, Sweat & Tears
Mechanic and the Mania
```

Lines starting with `#` are ignored, so you can leave yourself notes. 

Write the name exactly as it appears in your tags, punctuation and all — that
is the string being matched.

It only changes where a credit is cut in two. A name in the file is never
invented as a guest; it only stops one being taken apart. And you do **not**
need to list an artist you own albums by — the player already keeps those
whole.

---

## When it takes effect

The whole library's credits are worked out in one pass, the first time you open
Music after switching on. It takes a moment and then costs nothing until
something changes. `known_artists.txt` is read in that same pass.

So an edit to the file applies at the **next boot**, and at any point the
library itself changes, such as after adding music.
