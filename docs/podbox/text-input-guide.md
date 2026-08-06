# Text input

How to type on a click wheel. One editor is used everywhere text is entered —
renaming a file, naming a playlist, searching the database — so learning it once
covers all of them.

There is no on-screen keyboard grid to steer a cursor around. Instead there is a
single line of text with an insertion point in it: **the wheel picks the
character, the buttons move the caret.**

---

## The controls

| Control | In the text |
|---|---|
| **Wheel forward** | Insert a character at the caret, then cycle it forward |
| **Wheel back** | Same, cycling backward |
| **Tap Right** | Move the caret one place right, keeping the character |
| **Tap Left** | Move the caret one place left, keeping the character |
| **Hold Right** | Delete the character *after* the caret, repeating |
| **Hold Left** | Backspace the character *before* the caret, repeating |
| **SELECT** (centre) | Accept the text |
| **PLAY** (bottom) | Move down, out of the text — to the buttons, or to the results |
| **MENU** (top) | Move back up into the text; from the text, cancel |

Nothing is modal and nothing is chorded: the wheel always means "change this
character", and the two buttons either side of it always mean "in" and "out".

---

## The caret: bar and block

The caret is an insertion point **between** characters, drawn as a thin vertical
bar. It is not a block sitting on a character.

The two shapes tell you what the wheel will do next:

| Shape                      | Meaning |
|----------------------------|---|
| **Bar** between characters | The next spin **inserts a new character** here |
| **Block** on a character   | The wheel is **still changing that character** |

So spinning the wheel opens a gap and starts composing a character in it; the
block marks the one you are composing. Tapping Left or Right **commits** it —
the block becomes a bar again, and the next spin inserts a fresh character
rather than overwriting the one you just chose.

---

## The character cycle

The wheel steps through one fixed cycle, which wraps in both directions:

```
(space) A B C D E F G H I J K L M N O P Q R S T U V W X Y Z 0 1 2 3 4 5 6 7 8 9 - _ . ' &
```

Space comes first, so one click forward from a fresh gap gives a space and one
click back gives `&`. **There is no lowercase.** Filenames and database matching
are both case-insensitive here, so a second pass of 26 letters would only cost
spins.

**Spin faster to move faster.** Quick wheel steps skip 2 characters, very quick
ones skip 4, so crossing the alphabet is a flick rather than 26 clicks. Slow
down as you approach the letter you want.

Leading and trailing spaces are trimmed when you accept — except in Search,
where a space is a real search character and is kept as typed.

Translations can replace the cycle with their own characters and order; the set
above is the English one.

---

## The Cancel / OK row

Everywhere except Search, the editor sits above a **Cancel** and an **OK**
button.

- **SELECT** while the text has focus accepts immediately. You never have to
  visit the buttons.
- **PLAY** moves focus down to the row, starting on **OK**. The wheel and the
  Left/Right taps now pick between the two buttons instead of editing.
- **SELECT** presses the chosen button.
- **MENU** moves focus back up to the text.
- **MENU** from the text cancels. If you have changed anything, it asks
  **"Discard changes?"** first; answering no returns you to the text with your
  edits intact.

Plugging in USB closes the editor and discards the edit.

---

## Search in Music

Search puts the same editor above a live list of database matches. It is the
best place to get a feel for the control, because results appear as you type.

**Finding it.** It sits in the **Music** list directly above *Search by…*. It
can also be added to the main menu (*Settings → Main Menu Settings*). Both
entries are hidden unless the database is loaded into RAM — the scan reads every tag, which
from disk is unusably slow.

The search runs about a second after you stop turning the wheel, not on every
character, so a long query costs one scan rather than twenty.

**What is matched.** Case-insensitive, anywhere in the text — `beat` finds
*Heartbeat*. Three tags are searched, and the results are grouped in this order:

| Icon | Tag | Selecting it |
|---|---|---|
| Track | Track title | Plays that track |
| Album | Album | Opens that album's tracks |
| Artist | Album artist | Opens that artist's albums |

Per-track artist is deliberately not searched.

### Moving around

- **PLAY** moves down from the query into the results.
- In the results the wheel moves the selection; the selected row is drawn in the
  accent colour and its text scrolls if it is too long.
- **SELECT** opens the selected row.
- **MENU** goes back up to the query; **MENU** again closes Search.

**Between visits.** The query is remembered until the player reboots, so
reopening Search resumes where you left off and re-runs itself a moment later.

The query is capped at 32 characters and the first 200 matches are kept.