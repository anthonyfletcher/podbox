/* dbfeat -- checks the guest-credit parser, on the host.
 *
 * apps-ipod/database/db_featured.c turns "Song (feat. X)" into the names it
 * credits. Every one of its rules is a guess about how people write tags, and
 * a wrong guess shows up as a browser row naming somebody who does not exist
 * -- which is a slow thing to notice on a player and an instant one here.
 *
 * This compiles and calls the real parser. There is no mirrored copy of it to
 * drift, and the only thing it stands in for is the library: 'known' below
 * answers out of a fixed list of artists instead of the tag database.
 *
 * See tools/dbfeat/README.md.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "db_featured.h"

/* ------------------------------------------------------------------ *
 * the pretend library                                                *
 * ------------------------------------------------------------------ */

/* The artists this library already has. Which names are in here changes the
 * answer -- that is the whole point of the callback -- so it is stocked to
 * put both sides of that question in the case table below: a band whose
 * singer has no entry of his own, and one whose singer does. */
static const char * const library[] = {
    "Florence and the Machine",
    "Nick Cave & the Bad Seeds",
    "Nick Cave",
    "Earth, Wind & Fire",
    "AC/DC",
    "Drake",
    "Rihanna",
    "Kylie Minogue",
    "Jay-Z",
    "Tyler, The Creator",
    "Guest One",
    "Guest Two",
};

#define LIBRARY_COUNT ((int)(sizeof(library) / sizeof(library[0])))

static bool known(const char *name, int len, void *ctx)
{
    (void)ctx;

    for (int i = 0; i < LIBRARY_COUNT; i++)
        if (db_featured_name_eq(name, len, library[i], (int)strlen(library[i])))
            return true;

    return false;
}

/* ------------------------------------------------------------------ *
 * the cases                                                          *
 * ------------------------------------------------------------------ */

/* A string and the names it must yield, in order. The 'note' is printed for a
 * case whose expected answer is itself worth arguing about, so a run says so
 * rather than reading as a clean pass. */
struct testcase {
    const char *in;
    const char *out[DB_FEATURED_MAX_GUESTS + 1];
    const char *note;
};

static const struct testcase cases[] = {

/* --- the markers ------------------------------------------------- */
{ "Song (feat. Guest One)",          { "Guest One", NULL }, NULL },
{ "Song (Feat. Guest One)",          { "Guest One", NULL }, NULL },
{ "Song (FEAT. Guest One)",          { "Guest One", NULL }, NULL },
{ "Song (ft. Guest One)",            { "Guest One", NULL }, NULL },
{ "Song ft Guest One",               { "Guest One", NULL }, NULL },
{ "Song featuring Guest One",        { "Guest One", NULL }, NULL },
{ "Song w/ Guest One",               { "Guest One", NULL }, NULL },
{ "Song (feat.Guest One)",           { "Guest One", NULL }, NULL },
{ "feat. Guest One",                 { "Guest One", NULL }, NULL },
{ "Artist A feat. Artist B",         { "Artist B",  NULL }, NULL },

/* --- and what must not be one ------------------------------------ */
{ "Defeat the Silence",              { NULL }, NULL },
{ "Feather Light",                   { NULL }, NULL },
{ "Soft Machine",                    { NULL }, NULL },
{ "Aftermath",                       { NULL }, NULL },
{ "Song with a Broken Heart",        { NULL }, NULL },
{ "Song w/o Vocals",                 { NULL }, NULL },
{ "",                                { NULL }, NULL },

/* --- bracket scope ----------------------------------------------- */
{ "Song (feat. Guest One) [Remastered]",  { "Guest One", NULL }, NULL },
{ "Song [feat. Guest One] (Live)",        { "Guest One", NULL }, NULL },
{ "Song (Remix feat. Guest One)",         { "Guest One", NULL }, NULL },
{ "Song (feat. Guest One) (feat. Guest Two)",
  { "Guest One", NULL },
  "only the first marker in a string is read" },
{ "Song (feat. Guest One (Live))",        { "Guest One", NULL }, NULL },
{ "Song feat. Guest One",                 { "Guest One", NULL }, NULL },
{ "Song feat. Guest One (Live)",          { "Guest One", NULL }, NULL },

/* --- splitting --------------------------------------------------- */
{ "Song (feat. Drake & Rihanna)",     { "Drake", "Rihanna", NULL }, NULL },
{ "Song (feat. Drake, Rihanna)",      { "Drake", "Rihanna", NULL }, NULL },
{ "Song (feat. Drake; Rihanna)",      { "Drake", "Rihanna", NULL }, NULL },
{ "Song (feat. Drake x Rihanna)",     { "Drake", "Rihanna", NULL }, NULL },
{ "Song (feat. Jay-Z + Kylie Minogue)",
  { "Jay-Z", "Kylie Minogue", NULL }, NULL },
{ "Song (feat. Guest One & Guest Two)",
  { "Guest One", "Guest Two", NULL }, NULL },
{ "Song (feat. Drake, Rihanna & Jay-Z)",
  { "Drake", "Rihanna", "Jay-Z", NULL }, NULL },

/* --- and what must not split ------------------------------------- */
{ "Song (feat. Florence and the Machine)",
  { "Florence and the Machine", NULL }, NULL },
{ "Song (feat. AC/DC)",               { "AC/DC", NULL }, NULL },
{ "Song (feat. Earth, Wind & Fire)",  { "Earth, Wind & Fire", NULL }, NULL },
{ "Song (feat. Nick Cave & the Bad Seeds)",
  { "Nick Cave & the Bad Seeds", NULL }, NULL },
{ "Song (feat. Drake & Nick Cave & the Bad Seeds)",
  { "Drake", "Nick Cave & the Bad Seeds", NULL }, NULL },
{ "Song (feat. Nick Cave & the Bad Seeds & Drake)",
  { "Nick Cave & the Bad Seeds", "Drake", NULL }, NULL },

/* --- and what splits for want of anything better ----------------- */
{ "Song (feat. Unknown One, Unknown Two)",
  { "Unknown One", "Unknown Two", NULL }, NULL },
{ "Song (feat. Drake, Unknown Two)",  { "Drake", "Unknown Two", NULL }, NULL },

/* --- a name that is itself punctuated ---------------------------- */
{ "Song (feat. Tyler, The Creator)",
  { "Tyler, The Creator", NULL }, NULL },
{ "Song (feat. Tyler, The Creator & Drake)",
  { "Tyler, The Creator", "Drake", NULL }, NULL },
{ "Song (feat. Drake & Tyler, The Creator)",
  { "Drake", "Tyler, The Creator", NULL }, NULL },
{ "Song (feat. Some Band & Co)",
  { "Some Band", "Co", NULL },
  "a guest band the library has never heard of comes apart" },

/* --- what gets dropped ------------------------------------------- */
{ "Song (feat. )",                    { NULL }, NULL },
{ "Song (feat. A)",                   { NULL }, NULL },
{ "Song (feat. A & B)",               { NULL }, NULL },
{ "Song (feat. Drake & A)",           { "Drake", NULL }, NULL },
{ "Song (feat. Aaaaaaaaaa Bbbbbbbbbb Cccccccccc Dddddddddd "
  "Eeeeeeeeee Ffffffffff Gggg)",      { NULL }, NULL },
};

#define CASE_COUNT ((int)(sizeof(cases) / sizeof(cases[0])))

/* ------------------------------------------------------------------ *
 * running them                                                       *
 * ------------------------------------------------------------------ */

static void print_names(const struct db_featured_names *n)
{
    if (n->count == 0)
        printf("(nothing)");

    for (int i = 0; i < n->count; i++)
        printf("%s[%.*s]", i ? " " : "", n->len[i], n->name[i]);
}

/* Whether the parse of 'c' matched, printing it if not or if 'verbose'. */
static bool run_case(const struct testcase *c, bool verbose)
{
    struct db_featured_names got;
    int want = 0;
    bool ok = true;

    db_featured_parse(c->in, known, NULL, &got);

    while (c->out[want] != NULL)
        want++;

    if (got.count != want)
        ok = false;
    else
        for (int i = 0; i < want; i++)
            if ((int)strlen(c->out[i]) != got.len[i] ||
                memcmp(c->out[i], got.name[i], got.len[i]) != 0)
                ok = false;

    if (ok && !verbose)
        return true;

    printf("%s \"%s\"\n", ok ? "  ok  " : "FAIL  ", c->in);
    printf("          got  ");
    print_names(&got);
    printf("\n");

    if (!ok)
    {
        printf("          want ");
        if (want == 0)
            printf("(nothing)");
        for (int i = 0; i < want; i++)
            printf("%s[%s]", i ? " " : "", c->out[i]);
        printf("\n");
    }

    return ok;
}

/* One string per line from stdin, for trying the parser on real titles. */
static void run_stdin(void)
{
    char line[512];

    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        struct db_featured_names got;
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        db_featured_parse(line, known, NULL, &got);

        if (got.count == 0)
            continue;

        printf("%-50s ", line);
        print_names(&got);
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    bool verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    int failed = 0;
    int noted = 0;

    if (argc > 1 && strcmp(argv[1], "-") == 0)
    {
        run_stdin();
        return 0;
    }

    for (int i = 0; i < CASE_COUNT; i++)
        if (!run_case(&cases[i], verbose))
            failed++;

    for (int i = 0; i < CASE_COUNT; i++)
    {
        if (cases[i].note == NULL)
            continue;
        if (noted++ == 0)
            printf("\nexpected, and worth arguing about:\n");
        printf("  \"%s\"\n      %s\n", cases[i].in, cases[i].note);
    }

    printf("\n%d cases, %d failed\n", CASE_COUNT, failed);

    return failed != 0;
}
