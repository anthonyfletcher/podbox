/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to theme_options.c: a theme's .options files, and the
 * .preferences files they describe.
 ****************************************************************************/

#ifndef _THEME_OPTIONS_H_
#define _THEME_OPTIONS_H_

#include <stdbool.h>

#define THEME_OPTION_KEY_MAX     32     /* longest key, in bytes */
#define THEME_OPTION_NAME_MAX    40     /* longest name, label or title */
#define THEME_OPTION_HELP_MAX    1023   /* longest help, Explain's limit */
#define THEME_OPTION_VALUE_MAX   32     /* longest choice value */
#define THEME_OPTION_TEXT_MAX    120    /* longest text value */
#define THEME_OPTION_CHOICES     16     /* choices per option */
#define THEME_OPTIONS_MAX        32     /* options per file */
#define THEME_OPTIONS_FILES      4      /* .preferences files per theme */
#define THEME_OPTIONS_LIVE       64     /* distinct live %ft reads */

enum theme_option_type
{
    THEME_OPTION_CHOICE = 1,
    THEME_OPTION_TEXT,
};

struct theme_option_choice
{
    char value[THEME_OPTION_VALUE_MAX + 1];
    char label[THEME_OPTION_NAME_MAX + 1];
};

struct theme_option
{
    char key[THEME_OPTION_KEY_MAX + 1];
    char name[THEME_OPTION_NAME_MAX + 1];
    char help[THEME_OPTION_HELP_MAX + 1];
    char def[THEME_OPTION_TEXT_MAX + 1];
    unsigned char type;             /* enum theme_option_type */
    unsigned char max;              /* a text value's longest */
    unsigned char count;            /* choices */
    struct theme_option_choice choices[THEME_OPTION_CHOICES];
};

/* An options file's own fields */
struct theme_options_head
{
    char title[THEME_OPTION_NAME_MAX + 1];
    char help[THEME_OPTION_HELP_MAX + 1];
    char toggle[THEME_OPTION_KEY_MAX + 1];  /* a good choice option, or "" */
};

/* Calls `each` for every good option in the options file at `path`, in file
 * order, and fills `head` if it is not NULL. Returns the number of good
 * options, or -1 for a file that is missing or not format 1. Errors go to
 * the skin debug log. */
int theme_options_parse(const char *path, struct theme_options_head *head,
                        void (*each)(const struct theme_option *opt,
                                     void *data),
                        void *data);

/* The value `key` has in the .preferences file at `path`, as %ft reads it,
 * into `buf`; false if it has no line or the file cannot be read */
bool theme_options_read(const char *path, const char *key,
                        char *buf, int bufsize);

/* What %ft finds in the file at `path`: the rest of the first line that
 * starts with the `search_len` bytes of `search`, after any spaces, or with
 * `search` NULL, line `line` from 0. Into `buf`, cut to fit; returns its
 * strlen(), or -1 when nothing was found or it is empty. */
int theme_options_filetext(const char *path, const char *search,
                           int search_len, int line, char *buf, int bufsize);

/* Whether `value` is one `opt` may have */
bool theme_option_valid(const struct theme_option *opt, const char *value);

/* Sets `count` keys of the .preferences file at `path` to `values`: the
 * first line of each key is replaced and a missing key appended, through
 * <path>.new. Returns false if the file was not changed. */
bool theme_options_write(const char *path, const char * const *keys,
                         const char * const *values, int count);

/* Forgets the .preferences files recorded, at the start of a theme load */
void theme_options_forget(void);

/* A skin's %ft reads `path`: the first time in a load, records it and
 * completes it from the options file beside it */
void theme_options_met(const char *path);

/* A skin's %ft found `value` in `path`, as theme_options_filetext() takes
 * the search: an index for theme_options_live_value(), kept up to date by
 * every write, or -1 to keep the value fixed. Only a recorded file with
 * options is live. */
int theme_options_live(const char *path, const char *search, int search_len,
                       int line, const char *value);
const char *theme_options_live_value(int n);

/* Whether every %ft that reads `path` is live, so a write to it needs no
 * reload */
bool theme_options_live_only(const char *path);

/* The recorded .preferences files whose options files have a good option:
 * how many, and the n-th one's path and title */
int theme_options_count(void);
const char *theme_options_path(int n);
const char *theme_options_title(int n);
/* The n-th one's choice option for Theme Toggle, from its options file's
 * `toggle`, or "" */
const char *theme_options_toggle(int n);

/* The .options path for a .preferences path, into `buf` */
bool theme_options_options_path(const char *prefs, char *buf, int bufsize);

/* CheckWPS, in place of theme_options_met(): a skin's %ft reads `path`,
 * looking for the `keylen` bytes of `key` (NULL for a line number). Prints
 * the options file's errors the first time, and a key it does not define. */
void theme_options_check(const char *path, const char *key, int keylen);

#endif /* _THEME_OPTIONS_H_ */
