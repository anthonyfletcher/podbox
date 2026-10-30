/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Theme options: a theme's .options files, which describe the values its
 * .preferences files may hold, and the writes to those .preferences files.
 * .specifications/theme-options.md is the format.
 *
 * Parts:
 *   parse      theme_options_parse() streams an options file, one option
 *              held at a time, and hands each good one to its caller
 *   read       theme_options_read(): a key's value, found as %ft finds it
 *   write      theme_options_write(): a copy through <name>.new, changing
 *              only the keys' first lines, then the swap
 *   load       theme_options_met(), from the skin parser: records each
 *              .preferences file and completes it from its options
 *   check      theme_options_check(), CheckWPS's instead: prints each
 *              options file's errors, and each %ft key it does not define
 *
 * Nothing here allocates: completion runs inside the skin parse, where a
 * core_alloc() stops playback. The working state is static instead, and
 * everything here runs on the UI thread.
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "system.h"              /* MIN */
#include "pathfuncs.h"
#include "string-extra.h"
#include "system/strutil.h"      /* open_utf8, read_line */
#include "system/debug_log.h"
#include "theme_options.h"

/* A .preferences file past this is never touched, and gets no menu */
#define PREFS_LIMIT 16384

/* Longest line an options file may have */
#define OPTIONS_LINE 256

#define NEW_SUFFIX ".new"

static char line[OPTIONS_LINE];

static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = '\0';
    return s;
}

static bool has_control(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f)
            return true;
    return false;
}

/* Lower-case letters, digits, '-', '_', and spaces between words: Scrim's
 * keys ("sub-line mode") have spaces and its skins name them so */
static bool good_key(const char *key)
{
    size_t len = strlen(key);

    if (len == 0 || len > THEME_OPTION_KEY_MAX
        || key[0] == ' ' || key[len - 1] == ' ')
        return false;
    for (; *key; key++)
        if (!((*key >= 'a' && *key <= 'z') || (*key >= '0' && *key <= '9')
              || *key == '-' || *key == '_' || *key == ' '))
            return false;
    return true;
}

/* Joins `text` onto a help field with a space; false when it overflows */
static bool add_help(char *help, const char *text)
{
    size_t used = strlen(help);
    size_t len = strlen(text);

    if (used + (used > 0) + len > THEME_OPTION_HELP_MAX)
        return false;
    if (used > 0)
        help[used++] = ' ';
    memcpy(help + used, text, len + 1);
    return true;
}

bool theme_option_valid(const struct theme_option *opt, const char *value)
{
    if (opt->type == THEME_OPTION_CHOICE)
    {
        for (int i = 0; i < opt->count; i++)
            if (!strcmp(opt->choices[i].value, value))
                return true;
        return false;
    }
    return *value && strlen(value) <= opt->max && !has_control(value);
}

/* --- parse --------------------------------------------------------------- */

#define FIELD_NAME    0x01
#define FIELD_TYPE    0x02
#define FIELD_DEFAULT 0x04
#define FIELD_MAX     0x08

static struct
{
    const char *path;
    int line;                   /* the line being read */
    int head_line;              /* the option's heading */
    struct theme_option opt;    /* the option being read */
    bool open;                  /* an option is being read */
    bool bad;                   /* ...and has an error */
    unsigned fields;
    char seen[THEME_OPTIONS_MAX][THEME_OPTION_KEY_MAX + 1];
    int count;                  /* headings met */
} p;

/* An error goes to the theme log, or to CheckWPS's output */
static void report(int line, const char *what)
{
#ifdef CHECKWPS
    printf("%s:%d: %s\n", p.path, line, what);
#else
    debug_log(DEBUG_LOG_SKIN, "%s:%d: %s", p.path, line, what);
#endif
}

static void error(const char *what)
{
    report(p.line, what);
}

/* An error in the option being read: it is left out */
static void drop(const char *what)
{
    error(what);
    p.bad = true;
}

/* The option ends: checks what can only be checked whole, naming the
 * option's heading. True if good. */
static bool finish(void)
{
    struct theme_option *o = &p.opt;
    const char *why = NULL;

    if (!p.open || p.bad)
        return false;
    if ((p.fields & (FIELD_NAME | FIELD_TYPE | FIELD_DEFAULT))
        != (FIELD_NAME | FIELD_TYPE | FIELD_DEFAULT))
        why = "option lacks name, type or default";
    else if (o->type == THEME_OPTION_CHOICE)
    {
        if (p.fields & FIELD_MAX)
            why = "max on a choice";
        else if (o->count < 2)
            why = "fewer than two choices";
        else if (!theme_option_valid(o, o->def))
            why = "default is not a choice";
    }
    else if (o->count > 0)
        why = "choice on a text option";
    else if (!theme_option_valid(o, o->def))
        why = "default is empty or longer than max";

    if (why)
        report(p.head_line, why);
    return !why;
}

static void heading(char *s)
{
    size_t len = strlen(s);

    p.open = true;
    p.head_line = p.line;
    p.bad = false;
    p.fields = 0;
    memset(&p.opt, 0, sizeof p.opt);
    p.opt.max = 60;

    s[len - 1] = '\0';
    s = trim(s + 1);
    if (p.count++ >= THEME_OPTIONS_MAX)
    {
        drop("more than 32 options");
        return;
    }
    if (!good_key(s))
    {
        drop("bad key");
        return;
    }
    for (int i = 0; i < p.count - 1; i++)
    {
        if (!strcmp(p.seen[i], s))
        {
            drop("duplicate key");
            return;
        }
    }
    strcpy(p.seen[p.count - 1], s);
    strcpy(p.opt.key, s);
}

static void option_field(const char *field, char *value)
{
    struct theme_option *o = &p.opt;

    if (!strcmp(field, "name"))
    {
        if (strlen(value) > THEME_OPTION_NAME_MAX || !*value)
            drop("name empty or too long");
        else
            strcpy(o->name, value);
        p.fields |= FIELD_NAME;
    }
    else if (!strcmp(field, "help"))
    {
        if (!add_help(o->help, value))
            drop("help too long");
    }
    else if (!strcmp(field, "type"))
    {
        if (!strcmp(value, "choice"))
            o->type = THEME_OPTION_CHOICE;
        else if (!strcmp(value, "text"))
            o->type = THEME_OPTION_TEXT;
        else
            drop("type is not choice or text");
        p.fields |= FIELD_TYPE;
    }
    else if (!strcmp(field, "choice"))
    {
        char *label = strchr(value, '|');

        if (label)
        {
            *label++ = '\0';
            label = trim(label);
        }
        value = trim(value);
        if (!label || !*label)
            label = value;
        if (o->count >= THEME_OPTION_CHOICES)
            drop("more than 16 choices");
        else if (!*value || strlen(value) > THEME_OPTION_VALUE_MAX
                 || strchr(value, ' ') || strchr(value, '\t'))
            drop("choice value empty, too long or with spaces");
        else if (strlen(label) > THEME_OPTION_NAME_MAX)
            drop("choice label too long");
        else
        {
            strcpy(o->choices[o->count].value, value);
            strcpy(o->choices[o->count].label, label);
            o->count++;
        }
    }
    else if (!strcmp(field, "default"))
    {
        if (strlen(value) > THEME_OPTION_TEXT_MAX)
            drop("default too long");
        else
            strcpy(o->def, value);
        p.fields |= FIELD_DEFAULT;
    }
    else if (!strcmp(field, "max"))
    {
        char *end;
        long max = strtol(value, &end, 10);

        if (*end || max < 1 || max > THEME_OPTION_TEXT_MAX)
            drop("max is not 1 to 120");
        else
            o->max = max;
        p.fields |= FIELD_MAX;
    }
    /* Any other field belongs to a later format */
}

int theme_options_parse(const char *path, struct theme_options_head *head,
                        void (*each)(const struct theme_option *opt,
                                     void *data),
                        void *data)
{
    int fd, rd, format = 0, good = 0;
    bool head_bad = false;

    if (head)
        memset(head, 0, sizeof *head);
    fd = open_utf8(path, O_RDONLY);
    if (fd < 0)
        return -1;

    memset(&p, 0, sizeof p);
    p.path = path;

    while ((rd = read_line(fd, line, sizeof line)) > 0)
    {
        char *s = trim(line);
        char *eq;

        p.line++;
        if (strlen(line) >= sizeof line - 1)
            *s = '?';       /* a line cut short is an error line */

        if (!*s || *s == '#')
            continue;
        if (*s == '[' && s[strlen(s) - 1] == ']')
        {
            if (finish())
            {
                if (each)
                    each(&p.opt, data);
                good++;
            }
            else if (!p.open && (head_bad || format != 1))
            {
                if (format != 1)
                    error("format is not 1, or comes after an option");
                close(fd);
                return -1;
            }
            heading(s);
            continue;
        }

        eq = strchr(s, '=');
        if (!eq)
        {
            if (p.open)
                drop("not a heading, comment or field");
            else
            {
                error("not a heading, comment or field");
                head_bad = true;
            }
            continue;
        }
        *eq = '\0';
        char *field = trim(s);
        char *value = trim(eq + 1);

        if (p.open)
        {
            if (!p.bad)
                option_field(field, value);
        }
        else if (!strcmp(field, "format"))
            format = atoi(value);
        else if (!strcmp(field, "title"))
        {
            if (head && strlen(value) <= THEME_OPTION_NAME_MAX)
                strcpy(head->title, value);
        }
        else if (!strcmp(field, "help"))
        {
            if (head)
                add_help(head->help, value);
        }
    }
    close(fd);

    if (!p.open)
        return format == 1 && !head_bad ? 0 : -1;
    if (finish())
    {
        if (each)
            each(&p.opt, data);
        good++;
    }
    return good;
}

/* --- read ---------------------------------------------------------------- */

/* Whether `l` is the line %ft would find for `key` */
static bool key_line(const char *l, const char *key, size_t klen)
{
    return !strncasecmp(l, key, klen) && l[klen] == ':';
}

bool theme_options_read(const char *path, const char *key,
                        char *buf, int bufsize)
{
    char l[THEME_OPTION_KEY_MAX + THEME_OPTION_TEXT_MAX + 16];
    size_t klen = strlen(key);
    bool found = false;
    int fd = open_utf8(path, O_RDONLY);

    if (fd < 0)
        return false;
    while (read_line(fd, l, sizeof l) > 0)
    {
        if (key_line(l, key, klen))
        {
            const char *v = l + klen + 1;

            while (*v == ' ' || *v == '\t')
                v++;
            strmemccpy(buf, v, bufsize);
            found = true;
            break;
        }
    }
    close(fd);
    return found;
}


/* --- write --------------------------------------------------------------- */

static struct
{
    int fd;
    int used;
    bool failed;
    char last;                  /* the last byte written */
    unsigned char buf[512];
} out;

static void put(const void *data, int len)
{
    const unsigned char *d = data;

    if (len <= 0)
        return;
    out.last = d[len - 1];
    while (len > 0)
    {
        int n = MIN(len, (int)sizeof out.buf - out.used);

        memcpy(out.buf + out.used, d, n);
        out.used += n;
        d += n;
        len -= n;
        if (out.used == (int)sizeof out.buf)
        {
            if (write(out.fd, out.buf, out.used) != out.used)
                out.failed = true;
            out.used = 0;
        }
    }
}

static void put_line(const char *key, const char *value, bool crlf)
{
    put(key, strlen(key));
    put(": ", 2);
    put(value, strlen(value));
    put(crlf ? "\r\n" : "\n", crlf ? 2 : 1);
}

bool theme_options_write(const char *path, const char * const *keys,
                         const char * const *values, int count)
{
    char newpath[MAX_PATH];
    unsigned char in[256];
    char la[THEME_OPTION_KEY_MAX + 2];   /* a line's start, up to any ':' */
    char held[THEME_OPTION_KEY_MAX + 2]; /* a replaced line's own key */
    int la_len = 0, skipping = -1, src, n;
    unsigned done = 0;                   /* the keys whose line is written */
    bool start = true, cr = false, crlf = false, ending_known = false;

    if (snprintf(newpath, sizeof newpath, "%s" NEW_SUFFIX, path)
        >= (int)sizeof newpath)
        return false;

    src = open_utf8(path, O_RDONLY);
    if (src >= 0 && ffilesize(src) > PREFS_LIMIT)
    {
        close(src);
        return false;
    }
    out.fd = open(newpath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out.fd < 0)
    {
        if (src >= 0)
            close(src);
        return false;
    }
    out.used = 0;
    out.failed = false;
    out.last = '\n';
    if (src >= 0 && lseek(src, 0, SEEK_CUR) > 0)
        put("\xef\xbb\xbf", 3);         /* the byte-order mark it had */

    while (src >= 0 && (n = read(src, in, sizeof in)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            char c = in[i];

            if (c == '\n' && !ending_known)
            {
                crlf = cr;
                ending_known = true;
            }
            if (skipping >= 0)
            {
                if (c == '\n')
                {
                    put_line(held, values[skipping], cr);
                    skipping = -1;
                    start = true;
                }
                cr = (c == '\r');
                continue;
            }
            if (start)
            {
                /* A line's start is held until it shows whether the line
                 * is a key's */
                if (c != '\n' && c != ':' && la_len < THEME_OPTION_KEY_MAX + 1)
                {
                    la[la_len++] = c;
                    cr = (c == '\r');
                    continue;
                }
                start = false;
                la[la_len] = '\0';
                for (int k = 0; c == ':' && k < count; k++)
                {
                    if (!(done & (1u << k)) && !strcasecmp(la, keys[k]))
                    {
                        skipping = k;
                        done |= 1u << k;
                        strcpy(held, la);
                        break;
                    }
                }
                if (skipping < 0)
                    put(la, la_len);
                la_len = 0;
                if (skipping >= 0)
                {
                    cr = false;
                    continue;
                }
            }
            put(&c, 1);
            cr = (c == '\r');
            if (c == '\n')
                start = true;
        }
    }
    if (src >= 0)
        close(src);

    if (skipping >= 0)
        put_line(held, values[skipping], crlf);
    else if (start && la_len > 0)
        put(la, la_len);
    if (out.last != '\n')
        put(crlf ? "\r\n" : "\n", crlf ? 2 : 1);
    for (int k = 0; k < count; k++)
        if (!(done & (1u << k)))
            put_line(keys[k], values[k], crlf);

    if (out.used > 0 && write(out.fd, out.buf, out.used) != out.used)
        out.failed = true;
    if (close(out.fd) < 0)
        out.failed = true;
    if (out.failed)
    {
        remove(newpath);
        return false;
    }

    /* FAT's rename does not replace a file. A power cut between the two
     * leaves only the .new, which the next load renames into place. */
    remove(path);
    return rename(newpath, path) == 0;
}

/* --- load ---------------------------------------------------------------- */

/* The .preferences files the loaded theme's skins read */
static struct
{
    char path[MAX_PATH];
    char title[THEME_OPTION_NAME_MAX + 1];
    bool good;                  /* its options file has a good option */
} rec[THEME_OPTIONS_FILES];
static int recs;

/* What completion changes, in theme_options_write()'s terms */
static struct
{
    const char *prefs;
    int count;
    char keys[THEME_OPTIONS_MAX][THEME_OPTION_KEY_MAX + 1];
    char values[THEME_OPTIONS_MAX][THEME_OPTION_TEXT_MAX + 1];
    const char *kp[THEME_OPTIONS_MAX];
    const char *vp[THEME_OPTIONS_MAX];
} fix;

static struct theme_options_head head;

static void complete_one(const struct theme_option *opt, void *data)
{
    char v[THEME_OPTION_TEXT_MAX + 2];
    int n = fix.count;

    (void)data;
    if (theme_options_read(fix.prefs, opt->key, v, sizeof v)
        && theme_option_valid(opt, v))
        return;
    strcpy(fix.keys[n], opt->key);
    strcpy(fix.values[n], opt->def);
    fix.kp[n] = fix.keys[n];
    fix.vp[n] = fix.values[n];
    fix.count++;
}

static const char prefs_ext[] = ".preferences";
#define PREFS_EXT_LEN (sizeof prefs_ext - 1)

bool theme_options_options_path(const char *prefs, char *buf, int bufsize)
{
    size_t len = strlen(prefs) - PREFS_EXT_LEN;

    return snprintf(buf, bufsize, "%.*s.options", (int)len, prefs) < bufsize;
}

void theme_options_forget(void)
{
    recs = 0;
}

void theme_options_met(const char *path)
{
    char opts[MAX_PATH];
    char *title;
    size_t len = strlen(path);
    int fd;

    if (len <= PREFS_EXT_LEN || len >= MAX_PATH
        || strcasecmp(path + len - PREFS_EXT_LEN, prefs_ext))
        return;
    for (int i = 0; i < recs; i++)
        if (!strcasecmp(rec[i].path, path))
            return;
    if (recs == THEME_OPTIONS_FILES)
        return;
    strcpy(rec[recs].path, path);
    rec[recs].good = false;
    title = rec[recs].title;
    recs++;

    /* A write that stopped short: its .new is either the file, or nothing */
    if (snprintf(opts, sizeof opts, "%s" NEW_SUFFIX, path) < (int)sizeof opts
        && file_exists(opts))
    {
        if (file_exists(path))
            remove(opts);
        else
            rename(opts, path);
    }

    if (!theme_options_options_path(path, opts, sizeof opts)
        || !file_exists(opts))
        return;
    fd = open(path, O_RDONLY);
    if (fd >= 0)
    {
        bool big = ffilesize(fd) > PREFS_LIMIT;

        close(fd);
        if (big)
        {
            debug_log(DEBUG_LOG_SKIN, "%s: over 16 KB, not touched", path);
            return;
        }
    }

    fix.prefs = path;
    fix.count = 0;
    if (theme_options_parse(opts, &head, complete_one, NULL) <= 0)
        return;
    if (fix.count > 0 && !theme_options_write(path, fix.kp, fix.vp, fix.count))
        debug_log(DEBUG_LOG_SKIN, "%s: cannot write", path);

    if (head.title[0])
        strcpy(title, head.title);
    else
    {
        const char *name = strrchr(path, '/');

        name = name ? name + 1 : path;
        snprintf(title, THEME_OPTION_NAME_MAX + 1, "%.*s",
                 (int)(strlen(name) - PREFS_EXT_LEN), name);
    }
    rec[recs - 1].good = true;
}

int theme_options_count(void)
{
    int n = 0;

    for (int i = 0; i < recs; i++)
        n += rec[i].good;
    return n;
}

static int nth_good(int n)
{
    for (int i = 0; i < recs; i++)
        if (rec[i].good && n-- == 0)
            return i;
    return -1;
}

const char *theme_options_path(int n)
{
    int i = nth_good(n);

    return i < 0 ? NULL : rec[i].path;
}

const char *theme_options_title(int n)
{
    int i = nth_good(n);

    return i < 0 ? NULL : rec[i].title;
}

#ifdef CHECKWPS
/* --- check --------------------------------------------------------------- */

/* The .preferences files met in this run, each with its options' good keys
 * and the undefined keys already reported */
static struct
{
    char path[MAX_PATH];
    char opts[MAX_PATH];
    bool has_options;
    int count, missing;
    char keys[THEME_OPTIONS_MAX][THEME_OPTION_KEY_MAX + 1];
    char reported[THEME_OPTIONS_MAX][THEME_OPTION_KEY_MAX + 1];
} chk[THEME_OPTIONS_FILES];
static int chks;

static void check_one(const struct theme_option *opt, void *data)
{
    int n = (intptr_t)data;

    strcpy(chk[n].keys[chk[n].count++], opt->key);
}

static bool key_listed(char (*keys)[THEME_OPTION_KEY_MAX + 1], int count,
                       const char *key, int keylen)
{
    for (int i = 0; i < count; i++)
        if ((int)strlen(keys[i]) == keylen
            && !strncasecmp(keys[i], key, keylen))
            return true;
    return false;
}

void theme_options_check(const char *path, const char *key, int keylen)
{
    size_t len = strlen(path);
    int n;

    if (len <= PREFS_EXT_LEN || len >= MAX_PATH
        || strcasecmp(path + len - PREFS_EXT_LEN, prefs_ext))
        return;
    for (n = 0; n < chks; n++)
        if (!strcasecmp(chk[n].path, path))
            break;
    if (n == chks)
    {
        if (chks == THEME_OPTIONS_FILES)
            return;
        chks++;
        strcpy(chk[n].path, path);
        if (theme_options_options_path(path, chk[n].opts, MAX_PATH)
            && file_exists(chk[n].opts))
        {
            printf("Checking %s ...\n", chk[n].opts);
            chk[n].has_options =
                theme_options_parse(chk[n].opts, NULL, check_one,
                                    (void *)(intptr_t)n) >= 0;
        }
    }
    if (!key || !chk[n].has_options)
        return;

    /* %ft's search text is the key and its ':' */
    while (keylen > 0 && (key[keylen - 1] == ':' || key[keylen - 1] == ' '))
        keylen--;
    if (key_listed(chk[n].keys, chk[n].count, key, keylen)
        || key_listed(chk[n].reported, chk[n].missing, key, keylen))
        return;
    printf("%%ft key '%.*s': no such option in %s, or it has an error\n",
           keylen, key, chk[n].opts);
    if (chk[n].missing < THEME_OPTIONS_MAX && keylen <= THEME_OPTION_KEY_MAX)
        snprintf(chk[n].reported[chk[n].missing++], THEME_OPTION_KEY_MAX + 1,
                 "%.*s", keylen, key);
}
#endif /* CHECKWPS */
