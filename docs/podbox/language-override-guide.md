# Language overrides

**How to rename anything the player says, with a text file and no rebuild.**

If *Shuffle* should say *Randomise*, or *Settings* should say *Preferences*, put
a line in one file on the player and it is renamed everywhere at once — in the
menu, in the setting's own screen, in the context menu, and in any theme that
asks for that text by name.

This is not a translation system. It edits the English wording of individual
phrases, one line each, and leaves the other thousand-odd alone. For a whole
language you want a `.lng` file instead, from *Settings → System → Language & Text →
Language*.

---

## 1. The file

Create `/.rockbox/langs/overrides.cfg` on the player. The `langs` folder is
already there; the file is not, and nothing complains if it stays missing.

One rename per line, the current English text on the left and yours on the
right:

```
# my renames
Shuffle: Randomise
Settings: Preferences
Recently Added: New Music
Now Playing: Playing
```

That is the whole format. The text on the left is matched against the phrase
the player was built with, so **type it exactly as the English build shows it**
— same capitals, same spelling, no trailing space.

| Rule | What it means |
|---|---|
| Separator | The **first** colon on the line. Everything after it is the new text |
| Space before the colon | Not allowed — `Shuffle : x` looks for a phrase called `Shuffle ` |
| Space after the colon | Ignored, as is space at either end of the line |
| Comments | A line whose first non-blank character is `#` |
| Blank lines | Ignored, as is any line with no colon at all |
| Case | Exact. `shuffle` does not match *Shuffle* |
| An empty replacement | `Shuffle:` blanks the label rather than leaving it alone |
| Line length | 255 characters; a longer line is cut and the rest misread |
| A bad line | Skipped in silence — the rest of the file still applies |
| `Date#2:` | Renames only the second phrase reading *Date* — see §3 |

Order does not matter, and the same phrase named twice takes the last one.

---

## 2. When it takes effect

The file is read when the player applies its settings, which is:

- **at boot**, always;
- **when you unplug USB** — so the quickest way to work is to edit the file with
  the player plugged in, eject, and read the new wording as it comes back;
- when a theme or a `.cfg` is loaded, and after *Reset Settings*.

There is **one gap**: picking a language from *Settings → System → Language &
Text → Language* loads the `.lng` and drops your overrides until the next
reboot. Loading a language resets every phrase to built-in first, and nothing
re-applies the file at that point. Reboot after changing language.

---

## 3. What it reaches

Every phrase the firmware owns, wherever it appears. Renaming *Shuffle* renames
it in the settings tree, in the playlist context menu and in the shortcuts list
— they are all the same phrase, and there is no way to change only one of them.

Themes come along for free. A skin that writes `%Sx(Shuffle)` is naming that
same phrase, so it renders your replacement without being touched. See
[`custom-skin-tags.md`](custom-skin-tags.md).

**Keep any `%s` and `%d` exactly as they are, and in the same order.** They are
blanks the player fills in when the phrase is shown — `%d` with a number, `%s`
with a word. Reword the sentence around them as much as you like; just do not
drop one, add one, retype one, or swap two of them over:

```
Saved %d tracks (%s): Wrote %d tracks to (%s)   # fine
Saved %d tracks (%s): Wrote the tracks to (%s)  # dropped  -- line ignored
Saved %d tracks (%s): Wrote %d of %d to (%s)    # added    -- line ignored
Saved %d tracks (%s): Wrote %s tracks (%d)      # reordered -- line ignored
```

A line that gets it wrong is skipped and the built-in wording stays, so the
worst outcome is a rename that appears not to have worked.

**Write a literal per cent sign as `%%`.** A lone `%` is read as the start of
one of those blanks, so `50% done` counts as a blank and the line is skipped;
`50%% done` is what you want.

### Words the player uses twice

Twenty-one words are used by two separate phrases each — the same wording as a
settings label and again as a Track Info or properties row, say:

```
Album Art   Auto     Cancel     Custom   Date       Fast
Filename    Files    Games      Music    Normal     Path
Playlist    Search   Slow       Time     Track Gain Use UI Font
Rebuild Database     Update Database     Rescan Documents & Images
```

**Both are renamed.** `Album Art: Cover` changes the setting *and* the Track
Info row, which is almost always what was wanted — they read the same on screen
because they mean the same thing.

Where they do not, put a `#` and a number after the word to rename just one of
them:

```
Date#2: Modified
```

`#1` is the first of the two, `#2` the second, counted in the order the
firmware holds them — which is not the order you meet them on screen, so try
one and look. There is no way to tell them apart from the file other than by
trying.

The last three in that list are worth care. They are Maintenance rows, and a
row's explanation is looked up by the words on the row — so renaming one leaves
*Explain* on it blank.

---

## 4. What it cannot reach

Two kinds of text are out of reach, and the first is the one people notice.

**Messages built into the code.** A handful of brief pop-ups are written in
place rather than kept as phrases — *No supported files*, *Too large*,
*Database busy*, *Album Not Found!*, *Nothing Playing* and about a dozen more,
mostly from the image viewer. They have no name to put on the left of a line.

**A phrase whose English text contains a colon.** The line is split at its
first colon, so *Next Track:*, *Free:* and *Track Elapsed:* cannot be named. The
colon has to separate the two halves of the line, and there is no way to escape
one.

---

## 5. Under a translation

The left-hand side is **always English**, whatever language the player is set
to. It is matched against the phrases built into the firmware, which a `.lng`
never changes — so under Swedish you still write `Shuffle`, not `Blanda`, and
the replacement you give is used as-is.

Overrides share space with the loaded language, and a large translation leaves
less of it. An English player has room for roughly 27 KB of replacements, which
is hundreds of lines; a Bulgarian one has almost none. Once the space runs out
the remaining lines are dropped, from the point it filled up onwards, so put the
renames you care about most at the top of the file.

