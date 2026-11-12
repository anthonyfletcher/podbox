# Artwork guide

The README gives the simplest layout: a `folder.jpg` in each album's, book's and show's
folder, and one in each artist's or author's folder. This guide covers everything else
PodBox finds art from, for a library laid out another way or art you would rather keep
apart from your music. The same rules apply to music, audiobooks, books and podcasts.

After adding or moving art, run `Settings > Library > Maintenance > Update Art Cache`.

## One folder for all your art

An `Artwork` folder at the root of the player holds covers and pictures by name, and what
is in it wins over anything beside your files:

```text
Artwork
├── Covers
│   ├── Radiohead - OK Computer.jpg
│   ├── Dune.jpg
│   └── Show Name.jpg
└── People
    ├── Radiohead.jpg
    └── Frank Herbert.jpg
```

- In `Covers`, name a cover `Artist - Title` or just `Title`. For a book the artist is its
  author, and for a podcast its host. Use the artist when two works share a title.
- In `People`, name a picture after the artist or author.

`Files > Pictures` does not list the `Artwork` folder.

## How names are matched

Case, accents, punctuation, spaces and a leading "The" are ignored, `&` matches "and", and
"Herbert, Frank" matches "Frank Herbert". So `NERD.jpg` is the picture for N\*E\*R\*D and
N.E.R.D alike, `ACDC.jpg` for AC/DC, and a folder called `Beatles` is The Beatles'. This
applies to the `Artwork` files and to folder names alike.

An album's artist is its album artist tag, or its artist when there is none. Someone who is
both an artist and an author has one picture.

## Where a cover comes from

The first of:

1. the `Artwork/Covers` file named for it
2. for a book in one file, an image beside it with the book's name: `Dune Messiah.jpg` for
   `Dune Messiah.m4b`
3. `cover.jpg` or `folder.jpg` in its folder, and the cover embedded in its files

At step 3 the image file is used first and the embedded cover when there is none.
`Settings > Library > Art Cache > Album Art Source` changes that order, or limits it to one
kind; steps 1 and 2 are used whatever it says. An album in disc folders (`CD1`, `CD2`) can
keep its image in the folder above them. The README says which kinds of image work.

For music copied with iTunes, the art cache also reads thumbnails from
`iPod_Control/Artwork` when neither an image file nor embedded art supplies a
cover. Run Update Art Cache after syncing. This fallback is available with
either source order and with tags only; files only excludes it. It also applies
to audiobook tracks registered in iTunesDB. People's pictures still use image
files; standalone ebook covers follow the book index. The generated cache serves Now
Playing, lists and accessory artwork through their usual cache lookup.

This reads plain `iTunesDB`, `ArtworkDB` and RGB565 thumbnail formats 1055,
1060 and 1061. Compressed databases and other thumbnail layouts are not
supported. Indexing needs free memory proportional to the database size; when
that memory is unavailable, the job stops and keeps existing thumbnails until
a later attempt. Indexing starts only when a track needs the fallback.
It never borrows the audio buffer or writes to the iTunes databases.

A book's folder gives it a cover only when the book has the folder to itself. So with several
books loose in one folder, name each one's image after the book, as in step 2. A folder named
after the book's author is the author's, and its image is their picture, not the book's cover.

## Where a picture of a person comes from

Pictures only ever come from image files, never from tags. The first of:

1. the `Artwork/People` file named for them
2. `cover.jpg` or `folder.jpg` in a folder named after them: the tracks' own folder or up to
   three above it, so `Radiohead/folder.jpg` is found with the albums in `Radiohead` or in
   `Radiohead/Albums`
3. `cover.jpg` or `folder.jpg` in the folder above the album's, or the one above that. For an
   ebook, the book's own folder and the one above.

Step 3 is for an artist whose folder is not named the way the tags are, such as albums by
Prince and by Prince & The Revolution, all in `Prince`. It skips a folder that holds the
folders of more than one artist, such as `Music` or a genre folder, so an image there is
nobody's picture.

## When a cover is missing

`Settings > Library > Art Cache` has `Missing Album Artwork` and `Missing Artist Portraits`,
which list what has none. `Settings > System > Debug Logs > Album Art Log`
writes the reason for each to `art.log`.

## Now Playing

With `Settings > Appearance > Now Playing Screen > Album Art` set to `Prefer Image File`, Now
Playing reads the image itself rather than the cached copy: the `Artwork/Covers` file, an
image named after the track, or `cover.jpg` or `folder.jpg` in the track's folder. An
audiobook in a folder named after its author skips that folder's image, as above. Several
books loose in one folder all show the folder's image here, so name each one's image after
the book.
