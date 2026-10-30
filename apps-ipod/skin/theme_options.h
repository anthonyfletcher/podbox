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

/* The recorded .preferences files whose options files have a good option:
 * how many, and the n-th one's path and title */
int theme_options_count(void);
const char *theme_options_path(int n);
const char *theme_options_title(int n);

/* The .options path for a .preferences path, into `buf` */
bool theme_options_options_path(const char *prefs, char *buf, int bufsize);

#endif /* _THEME_OPTIONS_H_ */
