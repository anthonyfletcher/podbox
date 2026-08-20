# dbfeat — checking the guest-credit parser

`apps-ipod/database/db_featured_parse.c` reads guest credits out of prose: the
"(feat. X)" in a title, the "A feat. B" in a per-track artist tag. Every rule
it applies is a guess about how people write tags, and a wrong guess reaches
the user as a browser row naming somebody who does not exist. `dbfeat`
compiles the real parser on the host and runs it over a table of strings with
the names each must yield.

That file holds the parse and nothing else — no database, no allocation — so
it compiles here as it stands. Its sibling `db_featured.c` is the half that
builds the table out of tagcache. A parse rule that lands in the wrong one of
the two is a rule this test no longer covers.

Nothing here is built or shipped, and there is no copy of the parser to drift:
the only thing stood in for is the library. `db_featured_parse()` asks a
callback whether a fragment is already an artist in its own right — that is
what keeps "Nick Cave & the Bad Seeds" in one piece — and here the callback
answers out of a fixed list at the top of `dbfeat.c` instead of the tag
database.

```sh
gcc -O2 -W -Wall -Wextra -std=gnu11 -Iapps-ipod/database \
    -o dbfeat tools/dbfeat/dbfeat.c apps-ipod/database/db_featured_parse.c

./dbfeat            # the table; exit status 0 only if every case matched
./dbfeat -v         # print the passes too
./dbfeat -          # one string per line from stdin, for real titles
```

On Windows there is no `gcc` on `PATH`, but CLion ships one — put
`…\CLion <version>\bin\mingw\bin` at the front of `PATH` first, or it exits 1
and prints nothing at all.

## The two noted cases

A clean run still prints two cases under **expected, and worth arguing
about**. They pass: what they record is that the answer the parser gives is a
choice someone made, not the only possible one.

Only the first marker in a string is read, so "Song (feat. A) (feat. B)"
credits A alone. And a credit the library can say nothing at all about is
split anyway — most guests have no entry of their own, so two names is the
better guess than one long one, at the price of a guest band nobody has heard
of coming apart at its ampersand.

## Trying it on a real library

The stdin mode takes titles as they are and prints only the ones that credit
somebody, so a whole library's worth of titles reduces to the lines worth
looking at:

```sh
./dbfeat - < titles.txt
```

The pretend library will not match the real one, which matters only for the
splitting cases — add the artists that come up to `library[]` in `dbfeat.c` to
see what the player would do.
