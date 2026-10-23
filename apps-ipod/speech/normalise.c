/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Text to words for the speech synthesiser: a title, artist or menu string
 * becomes clauses of lowercase English words that lexicon.c can pronounce.
 *
 * Numbers become words (years in pairs, times, ordinals, decimals); a few
 * symbols and the abbreviations music titles use are read out; a short
 * capitalised or vowelless word the dictionary lacks is spelled; Roman
 * numerals of I, V and X become numbers. Accented Latin is folded to ASCII,
 * Greek, Cyrillic and kana are transliterated, and a run of a script with no
 * reading here -- Han, Hangul, Arabic, Hebrew, Thai -- is said as the
 * script's name: "japanese" when the text has kana, "chinese" when it has
 * Han and none.
 *
 * A clause ends at . ? ! , ; : before a space, at a bracket and at a dash
 * with spaces round it. A full stop after an abbreviation or an initial
 * ends nothing.
 *
 * Parts, in order:
 *   - the output buffer
 *   - characters: decoding, folding, transliteration, scripts
 *   - numbers
 *   - words
 *   - the public interface
 ****************************************************************************/

#include <stdint.h>
#include <string.h>

#include "rbunicode.h"
#include "lexicon.h"
#include "normalise.h"

#define WORD_MAX 64

static const unsigned char *pos;    /* the next clause starts here */
static bool text_has_kana;

/** The output buffer **/

struct out
{
    char *buf, *at, *end;
    bool full;
};

static void put_word(struct out *o, const char *w, int len)
{
    if (len <= 0 || o->full)
        return;
    if (o->at + (o->at > o->buf) + len > o->end)
    {
        o->full = true;
        return;
    }
    if (o->at > o->buf)
        *o->at++ = ' ';
    memcpy(o->at, w, len);
    o->at += len;
    *o->at = '\0';
}

static void put(struct out *o, const char *w)
{
    put_word(o, w, strlen(w));
}

/* Each letter of w by its name: the dictionary's "b." is "bee". */
static void put_spelled(struct out *o, const char *w, int len)
{
    for (int i = 0; i < len; i++)
    {
        if (w[i] < 'a' || w[i] > 'z')
            continue;
        char name[2] = { w[i], '.' };
        put_word(o, name, 2);
    }
}

/** Characters **/

static ucschar_t peek(const unsigned char *p, const unsigned char **next)
{
    ucschar_t c;
    *next = utf8decode(p, &c);
    return c;
}

/* U+00C0 to U+017F as ASCII, from their Unicode decompositions */
static const char *const latin_fold[192] =
{
    "a", "a", "a", "a", "a", "a", "ae", "c",
    "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "",
    "o", "u", "u", "u", "u", "y", "th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c",
    "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "",
    "o", "u", "u", "u", "u", "y", "th", "y",
    "a", "a", "a", "a", "a", "a", "c", "c",
    "c", "c", "c", "c", "c", "c", "d", "d",
    "d", "d", "e", "e", "e", "e", "e", "e",
    "e", "e", "e", "e", "g", "g", "g", "g",
    "g", "g", "g", "g", "h", "h", "h", "h",
    "i", "i", "i", "i", "i", "i", "i", "i",
    "i", "i", "ij", "ij", "j", "j", "k", "k",
    "k", "l", "l", "l", "l", "l", "l", "l",
    "l", "l", "l", "n", "n", "n", "n", "n",
    "n", "n", "ng", "ng", "o", "o", "o", "o",
    "o", "o", "oe", "oe", "r", "r", "r", "r",
    "r", "r", "s", "s", "s", "s", "s", "s",
    "s", "s", "t", "t", "t", "t", "t", "t",
    "u", "u", "u", "u", "u", "u", "u", "u",
    "u", "u", "u", "u", "w", "w", "y", "y",
    "y", "z", "z", "z", "z", "z", "z", "s",
};

struct translit
{
    uint16_t cp;
    char latin[5];
};

/* Modern Greek, with the accented vowels */
static const struct translit greek[] =
{
    { 0x386, "a" }, { 0x388, "e" }, { 0x389, "i" }, { 0x38a, "i" },
    { 0x38c, "o" }, { 0x38e, "y" }, { 0x38f, "o" }, { 0x390, "i" },
    { 0x391, "a" }, { 0x392, "v" }, { 0x393, "g" }, { 0x394, "d" },
    { 0x395, "e" }, { 0x396, "z" }, { 0x397, "i" }, { 0x398, "th" },
    { 0x399, "i" }, { 0x39a, "k" }, { 0x39b, "l" }, { 0x39c, "m" },
    { 0x39d, "n" }, { 0x39e, "x" }, { 0x39f, "o" }, { 0x3a0, "p" },
    { 0x3a1, "r" }, { 0x3a3, "s" }, { 0x3a4, "t" }, { 0x3a5, "y" },
    { 0x3a6, "f" }, { 0x3a7, "ch" }, { 0x3a8, "ps" }, { 0x3a9, "o" },
    { 0x3aa, "i" }, { 0x3ab, "y" }, { 0x3ac, "a" }, { 0x3ad, "e" },
    { 0x3ae, "i" }, { 0x3af, "i" }, { 0x3b0, "y" }, { 0x3b1, "a" },
    { 0x3b2, "v" }, { 0x3b3, "g" }, { 0x3b4, "d" }, { 0x3b5, "e" },
    { 0x3b6, "z" }, { 0x3b7, "i" }, { 0x3b8, "th" }, { 0x3b9, "i" },
    { 0x3ba, "k" }, { 0x3bb, "l" }, { 0x3bc, "m" }, { 0x3bd, "n" },
    { 0x3be, "x" }, { 0x3bf, "o" }, { 0x3c0, "p" }, { 0x3c1, "r" },
    { 0x3c2, "s" }, { 0x3c3, "s" }, { 0x3c4, "t" }, { 0x3c5, "y" },
    { 0x3c6, "f" }, { 0x3c7, "ch" }, { 0x3c8, "ps" }, { 0x3c9, "o" },
    { 0x3ca, "i" }, { 0x3cb, "y" }, { 0x3cc, "o" }, { 0x3cd, "y" },
    { 0x3ce, "o" },
};

/* Russian, with the Ukrainian and Belarusian letters */
static const char cyrillic[64][5] =
{
    "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n",
    "o", "p", "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "",
    "y", "", "e", "yu", "ya",
    "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n",
    "o", "p", "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "",
    "y", "", "e", "yu", "ya",
};

/* The Latin letters of a Latin, Greek or Cyrillic letter, or NULL. */
static const char *latin_of(ucschar_t c, char *tmp, bool *upper)
{
    *upper = false;
    if (c >= 'a' && c <= 'z')
    {
        tmp[0] = c;
        tmp[1] = '\0';
        return tmp;
    }
    if (c >= 'A' && c <= 'Z')
    {
        *upper = true;
        tmp[0] = c - 'A' + 'a';
        tmp[1] = '\0';
        return tmp;
    }
    if (c >= 0xc0 && c < 0x180)
    {
        *upper = c < 0xdf || (c >= 0x100 && !(c & 1));
        return *latin_fold[c - 0xc0] ? latin_fold[c - 0xc0] : NULL;
    }
    if (c >= 0x386 && c <= 0x3ce)
    {
        *upper = c <= 0x3ab;
        for (unsigned i = 0; i < sizeof(greek) / sizeof(*greek); i++)
            if (greek[i].cp == c)
                return greek[i].latin;
        return NULL;
    }
    if (c >= 0x410 && c <= 0x44f)
    {
        *upper = c < 0x430;
        return cyrillic[c - 0x410];
    }
    switch (c)
    {
    case 0x401: case 0x451: return "yo";
    case 0x404: case 0x454: return "ye";
    case 0x406: case 0x456: return "i";
    case 0x407: case 0x457: return "yi";
    case 0x40e: case 0x45e: return "u";
    case 0x490: case 0x491: return "g";
    }
    return NULL;
}

static bool is_apostrophe(ucschar_t c)
{
    return c == '\'' || c == 0x2019 || c == 0x2018;
}

static bool is_kana(ucschar_t c)
{
    return (c >= 0x3041 && c <= 0x30ff) || (c >= 0xff66 && c <= 0xff9f);
}

static bool is_han(ucschar_t c)
{
    return (c >= 0x4e00 && c <= 0x9fff) || (c >= 0x3400 && c <= 0x4dbf)
           || (c >= 0xf900 && c <= 0xfaff);
}

/* The script of a character with no reading here, or -1. Han alone
 * cannot tell Chinese from Japanese, so the text's kana decides. */
static int script_of(ucschar_t c, bool has_kana)
{
    if (is_han(c))
        return has_kana ? SCRIPT_JAPANESE : SCRIPT_CHINESE;
    if ((c >= 0xac00 && c <= 0xd7af) || (c >= 0x1100 && c <= 0x11ff)
        || (c >= 0x3130 && c <= 0x318f))
        return SCRIPT_KOREAN;
    if ((c >= 0x600 && c <= 0x6ff) || (c >= 0x750 && c <= 0x77f)
        || (c >= 0xfb50 && c <= 0xfdff) || (c >= 0xfe70 && c <= 0xfeff))
        return SCRIPT_ARABIC;
    if (c >= 0x590 && c <= 0x5ff)
        return SCRIPT_HEBREW;
    if (c >= 0xe00 && c <= 0xe7f)
        return SCRIPT_THAI;
    return -1;
}

static const char *const script_names[] =
{
    [SCRIPT_CHINESE] = "chinese", [SCRIPT_JAPANESE] = "japanese",
    [SCRIPT_KOREAN] = "korean", [SCRIPT_ARABIC] = "arabic",
    [SCRIPT_HEBREW] = "hebrew", [SCRIPT_THAI] = "thai",
};

/* The end of a run of one script from s, whose first character is of
 * `script` (or kana, -1): Han and kana run together, as Japanese, and CJK
 * punctuation stays in the run. *han is set if it has Han in it. */
static const unsigned char *script_run(const unsigned char *s, int script,
                                       bool has_kana, bool *han)
{
    *han = false;
    for (;;)
    {
        const unsigned char *n2;
        ucschar_t d = peek(s, &n2);
        if (d == 0)
            break;
        if (is_han(d))
            *han = true;
        else if (!is_kana(d) && (d < 0x3000 || d > 0x303f)
                 && (script < 0 || script_of(d, has_kana) != script))
            break;
        s = n2;
    }
    return s;
}

static bool has_kana_in(const unsigned char *p)
{
    while (*p)
    {
        const unsigned char *next;
        if (is_kana(peek(p, &next)))
            return true;
        p = next;
    }
    return false;
}

bool normalise_find_script(const char *utf8, const char **start,
                           const char **end, enum normalise_script *script)
{
    const unsigned char *p = (const unsigned char *)utf8;
    bool has_kana = has_kana_in(p);

    while (*p)
    {
        const unsigned char *next;
        ucschar_t c = peek(p, &next);
        int sc = script_of(c, has_kana);
        bool han;

        if (sc < 0 && !is_kana(c))
        {
            p = next;
            continue;
        }
        const unsigned char *e = script_run(p, sc, has_kana, &han);
        if (sc >= 0 || han)
        {
            *start = (const char *)p;
            *end = (const char *)e;
            *script = han ? (has_kana ? SCRIPT_JAPANESE : SCRIPT_CHINESE)
                          : (enum normalise_script)sc;
            return true;
        }
        p = e > p ? e : next;   /* kana alone: read as romaji */
    }
    return false;
}

/* Hiragana from U+3041 in Hepburn; katakana is 0x60 above. Small kana are
 * marked by a leading '_'. */
static const char *const kana[86] =
{
    "_a", "a", "_i", "i", "_u", "u", "_e", "e", "_o", "o",
    "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",
    "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo",
    "ta", "da", "chi", "ji", "_tsu", "tsu", "zu", "te", "de", "to", "do",
    "na", "ni", "nu", "ne", "no",
    "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu",
    "he", "be", "pe", "ho", "bo", "po",
    "ma", "mi", "mu", "me", "mo",
    "_ya", "ya", "_yu", "yu", "_yo", "yo",
    "ra", "ri", "ru", "re", "ro",
    "_wa", "wa", "wi", "we", "o", "n", "vu", "_ka", "_ke",
};

static bool is_vowel(char c)
{
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

/* A run of kana as one romaji word into w; returns its length. */
static int romaji(const unsigned char **p, char *w, int max)
{
    int n = 0;
    bool doubled = false;
    for (;;)
    {
        const unsigned char *next;
        ucschar_t c = peek(*p, &next);
        int i = -1;
        if (c >= 0x3041 && c <= 0x3096)
            i = c - 0x3041;
        else if (c >= 0x30a1 && c <= 0x30f6)
            i = c - 0x30a1;
        else if (c == 0x30fc)       /* the long mark: the vowel again */
        {
            if (n > 0 && n < max && is_vowel(w[n - 1]))
            {
                w[n] = w[n - 1];
                n++;
            }
            *p = next;
            continue;
        }
        else if (c == 0x30fb)       /* the middle dot ends the word */
        {
            *p = next;
            break;
        }
        else
            break;
        *p = next;

        const char *r = kana[i];
        if (r[0] == '_')
        {
            r++;
            if (!strcmp(r, "tsu"))
            {
                doubled = true;
                continue;
            }
            if (r[0] == 'y' && n > 0 && w[n - 1] == 'i')
            {
                n--;                /* ki + ya is kya, shi + ya is sha */
                if ((n >= 2 && w[n - 1] == 'h') || (n > 0 && w[n - 1] == 'j'))
                    r++;
            }
            else if (n > 0 && is_vowel(w[n - 1]) && is_vowel(r[0]))
                n--;                /* fu + small a is fa */
        }
        if (doubled && !is_vowel(r[0]) && n < max)
            w[n++] = r[0] == 'c' ? 't' : r[0];
        doubled = false;
        while (*r && n < max)
            w[n++] = *r++;
    }
    return n;
}

/** Numbers **/

static const char *const ones[20] =
{
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
    "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
    "sixteen", "seventeen", "eighteen", "nineteen",
};

static const char *const tens[10] =
{
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
    "eighty", "ninety",
};

/* 0 to 999 */
static void put_hundreds(struct out *o, unsigned n)
{
    if (n >= 100)
    {
        put(o, ones[n / 100]);
        put(o, "hundred");
        n %= 100;
        if (!n)
            return;
    }
    if (n < 20)
        put(o, ones[n]);
    else
    {
        put(o, tens[n / 10]);
        if (n % 10)
            put(o, ones[n % 10]);
    }
}

static void put_cardinal(struct out *o, uint64_t n)
{
    static const char *const scale[] = { "billion", "million", "thousand" };
    static const uint64_t unit[] = { 1000000000, 1000000, 1000 };
    if (n == 0)
    {
        put(o, "zero");
        return;
    }
    for (int i = 0; i < 3; i++)
    {
        if (n >= unit[i])
        {
            put_hundreds(o, n / unit[i]);
            put(o, scale[i]);
            n %= unit[i];
        }
    }
    if (n)
        put_hundreds(o, n);
}

/* 1100 to 2099 the way years are said */
static void put_year(struct out *o, unsigned y)
{
    if (y >= 2000 && y <= 2009)
    {
        put_cardinal(o, y);
        return;
    }
    put_hundreds(o, y / 100);
    if (y % 100 == 0)
        put(o, "hundred");
    else if (y % 100 < 10)
    {
        put(o, "oh");
        put(o, ones[y % 100]);
    }
    else
        put_hundreds(o, y % 100);
}

/* The last word written, made ordinal: two to second, twenty to twentieth */
static void make_ordinal(struct out *o)
{
    static const char *const irregular[][2] =
    {
        { "one", "first" }, { "two", "second" }, { "three", "third" },
        { "five", "fifth" }, { "eight", "eighth" }, { "nine", "ninth" },
        { "twelve", "twelfth" },
    };
    char *w = o->at;
    while (w > o->buf && w[-1] != ' ')
        w--;
    char last[16];
    int len = o->at - w;
    if (len <= 0 || len >= (int)sizeof(last))
        return;
    memcpy(last, w, len);
    last[len] = '\0';
    o->at = w > o->buf ? w - 1 : w;
    *o->at = '\0';
    for (unsigned i = 0; i < sizeof(irregular) / sizeof(*irregular); i++)
        if (!strcmp(last, irregular[i][0]))
        {
            put(o, irregular[i][1]);
            return;
        }
    char ord[20];
    if (last[len - 1] == 'y')                   /* twenty: twentieth */
    {
        memcpy(ord, last, len - 1);
        strcpy(ord + len - 1, "ieth");
    }
    else
    {
        memcpy(ord, last, len);
        strcpy(ord + len, "th");
    }
    put(o, ord);
}

static void put_digits(struct out *o, const char *d, int len)
{
    for (int i = 0; i < len; i++)
        put(o, ones[d[i] - '0']);
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* A number at *p: digits with thousands commas, a decimal point, a time's
 * colons or an ordinal's suffix. */
static void read_number(const unsigned char **p, struct out *o)
{
    const unsigned char *s = *p;
    char digits[24];
    int n = 0;
    bool commas = false;
    while (is_digit(*s) || (*s == ',' && is_digit(s[1]) && is_digit(s[2])
                            && is_digit(s[3]) && !is_digit(s[4])))
    {
        if (*s == ',')
            commas = true;
        else if (n < (int)sizeof(digits) - 1)
            digits[n++] = *s;
        s++;
    }
    digits[n] = '\0';

    uint64_t value = 0;
    for (int i = 0; i < n && i < 13; i++)
        value = value * 10 + digits[i] - '0';

    /* times: 3:42, 1:02:03 */
    if (*s == ':' && is_digit(s[1]) && is_digit(s[2]) && !is_digit(s[3]))
    {
        put_cardinal(o, value);
        while (*s == ':' && is_digit(s[1]) && is_digit(s[2]))
        {
            unsigned m = (s[1] - '0') * 10 + s[2] - '0';
            s += 3;
            if (m == 0 && *s != ':')
                put(o, "o'clock");
            else if (m < 10)
            {
                put(o, "oh");
                put(o, ones[m]);
            }
            else
                put_hundreds(o, m);
        }
        *p = s;
        return;
    }

    if (n > 12)
        put_digits(o, digits, n);
    else if (n == 4 && !commas && value >= 1100 && value <= 2099
             && value % 1000 != 0 && !(*s == '.' && is_digit(s[1])))
        put_year(o, value);
    else
        put_cardinal(o, value);

    /* 3.5, 1st, 22nd */
    if (*s == '.' && is_digit(s[1]))
    {
        s++;
        put(o, "point");
        while (is_digit(*s))
        {
            put(o, ones[*s - '0']);
            s++;
        }
    }
    else
    {
        static const char *const suffixes[] = { "st", "nd", "rd", "th" };
        for (int i = 0; i < 4; i++)
        {
            if ((s[0] | 0x20) == suffixes[i][0]
                && (s[1] | 0x20) == suffixes[i][1]
                && !((s[2] | 0x20) >= 'a' && (s[2] | 0x20) <= 'z'))
            {
                make_ordinal(o);
                s += 2;
                break;
            }
        }
    }
    *p = s;
}

/* I, V and X only, so that MIX and DIM stay words. 0 if not a numeral. */
static unsigned roman_value(const char *w, int len)
{
    unsigned total = 0;
    for (int i = 0; i < len; i++)
    {
        int v = w[i] == 'i' ? 1 : w[i] == 'v' ? 5 : w[i] == 'x' ? 10 : 0;
        if (!v)
            return 0;
        int next = i + 1 < len ? (w[i + 1] == 'i' ? 1 : w[i + 1] == 'v' ? 5
                                  : w[i + 1] == 'x' ? 10 : 0) : 0;
        total += v < next ? -v : v;
    }
    /* reject forms like IIII or VX that only parse by accident */
    static const char *const valid[] =
    {
        "ii", "iii", "iv", "vi", "vii", "viii", "ix", "xi", "xii", "xiii",
        "xiv", "xv", "xvi", "xvii", "xviii", "xix", "xx",
    };
    for (unsigned i = 0; i < sizeof(valid) / sizeof(*valid); i++)
        if ((int)strlen(valid[i]) == len && !memcmp(valid[i], w, len))
            return total;
    return 0;
}

/** Words **/

struct abbreviation
{
    const char *short_form, *words;
};

/* Said as words. Those marked '#' only before a number. */
static const struct abbreviation abbreviations[] =
{
    { "feat", "featuring" }, { "ft", "featuring" }, { "vol", "volume" },
    { "vols", "volumes" }, { "pt", "part" }, { "vs", "versus" },
    { "mr", "mister" }, { "mrs", "missus" }, { "ms", "miz" },
    { "dr", "doctor" }, { "st", "saint" }, { "jr", "junior" },
    { "sr", "senior" }, { "etc", "et cetera" }, { "no", "#number" },
    { "nos", "#numbers" }, { "op", "#opus" }, { "bwv", "#b w v" },
};

static const struct abbreviation *find_abbreviation(const char *w, int len)
{
    for (unsigned i = 0; i < sizeof(abbreviations) / sizeof(*abbreviations);
         i++)
    {
        const char *s = abbreviations[i].short_form;
        if ((int)strlen(s) == len && !memcmp(s, w, len))
            return &abbreviations[i];
    }
    return NULL;
}

static bool has_vowel(const char *w, int len)
{
    for (int i = 0; i < len; i++)
        if (is_vowel(w[i]) || w[i] == 'y')
            return true;
    return false;
}

/* The next character after spaces, without moving. */
static ucschar_t next_after_spaces(const unsigned char *p)
{
    while (*p == ' ')
        p++;
    const unsigned char *next;
    return peek(p, &next);
}

/* A word at *p: letters, apostrophes and hyphens between letters, and the
 * dots of an initialism. */
static void read_word(const unsigned char **p, struct out *o)
{
    char w[WORD_MAX];
    int n = 0, letters = 0, upper = 0;
    bool dotted = false;
    const unsigned char *s = *p;

    for (;;)
    {
        const unsigned char *next;
        ucschar_t c = peek(s, &next);
        char tmp[2];
        bool is_upper;
        const char *latin = latin_of(c, tmp, &is_upper);
        if (latin)
        {
            letters++;
            upper += is_upper;
            while (*latin && n < WORD_MAX - 1)
                w[n++] = *latin++;
            s = next;
            continue;
        }
        const unsigned char *after;
        ucschar_t following = peek(next, &after);
        char t2[2];
        bool u2;
        if ((is_apostrophe(c) || c == '-') && n > 0
            && latin_of(following, t2, &u2))
        {
            if (n < WORD_MAX - 1)
                w[n++] = c == '-' ? '-' : '\'';
            s = next;
            continue;
        }
        /* U.S.A. -- a dot after a lone letter that another letter follows */
        if (c == '.' && n > 0 && (n == 1 || w[n - 2] == '.')
            && latin_of(following, t2, &u2) && next[1] == '.')
        {
            dotted = true;
            if (n < WORD_MAX - 1)
                w[n++] = '.';
            s = next;
            continue;
        }
        break;
    }
    w[n] = '\0';

    /* a full stop that belongs to the word: an abbreviation, an initial */
    const struct abbreviation *abbr = find_abbreviation(w, n);
    bool stop = *s == '.';
    if (stop && (abbr || n == 1 || dotted))
    {
        s++;
        if (n == 1 || dotted)
            dotted = true;
    }
    *p = s;

    if (dotted)
    {
        put_spelled(o, w, n);
        return;
    }
    if (abbr)
    {
        const char *words = abbr->words;
        if (*words == '#')
        {
            if (is_digit(next_after_spaces(s)))
            {
                put(o, words + 1);
                return;
            }
        }
        else
        {
            put(o, words);
            return;
        }
    }
    bool all_caps = letters >= 2 && upper == letters;
    if (all_caps)
    {
        unsigned r = roman_value(w, n);
        if (r)
        {
            put_cardinal(o, r);
            return;
        }
    }
    if (n <= 5 && !memchr(w, '\'', n) && !memchr(w, '-', n)
        && (all_caps || !has_vowel(w, n)) && !lexicon_has(w, n))
    {
        put_spelled(o, w, n);
        return;
    }
    put_word(o, w, n);
}

/* The punctuation that ends a clause at p, consumed, with its type; or
 * false. */
static bool read_boundary(const unsigned char **p, int *clause, bool start)
{
    const unsigned char *next;
    ucschar_t c = peek(*p, &next);
    const unsigned char *s = next;
    switch (c)
    {
    case '.': case '?': case '!': case ',': case ';': case ':':
    case 0x2026: case 0x3002: case 0x3001: case 0xff01: case 0xff1f:
    case 0xff0c:
    {
        /* only before a space, the end, or more of the same */
        const unsigned char *after;
        ucschar_t f = peek(s, &after);
        if (f != ' ' && f != 0 && f != '\n'
            && !(f < 0x80 && strchr(".?!,;:", f))
            && f != ')' && f != ']' && f != '"' && f != 0x201d
            && c != 0x2026 && c < 0x3000)
            return false;
        while (*s && strchr(".?!,;:", *s))
            s++;
        *clause = c == '?' || c == 0xff1f ? '?'
                : c == '!' || c == 0xff01 ? '!'
                : c == '.' || c == 0x3002 ? '.'
                : c == ',' || c == 0x3001 || c == 0xff0c || c == 0x2026
                  ? ',' : 0;
        break;
    }
    case '(': case ')': case '[': case ']': case '{': case '}':
        *clause = ',';
        break;
    case '-': case 0x2013: case 0x2014:
        /* a dash alone between spaces, as in "Artist - Title" */
        if (!start || (*s != ' ' && *s != 0))
            return false;
        *clause = ',';
        break;
    case '\n': case '\r': case '\t':
        *clause = 0;
        break;
    default:
        return false;
    }
    *p = s;
    return true;
}

/** Interface **/

void normalise_begin(const char *utf8)
{
    pos = (const unsigned char *)utf8;
    text_has_kana = has_kana_in(pos);
}

bool normalise_next(char *buf, int size, int *clause)
{
    struct out o = { buf, buf, buf + size - 1, false };
    int last_script = -1;
    bool after_space = true;

    *buf = '\0';
    *clause = 0;
    while (*pos)
    {
        const unsigned char *token = pos;
        char *mark = o.at;
        const unsigned char *next;
        ucschar_t c = peek(pos, &next);
        char tmp[2];
        bool upper;

        if (c == ' ' || c == 0xa0 || c == 0x3000)
        {
            pos = next;
            after_space = true;
            continue;
        }
        int type;
        if (read_boundary(&pos, &type, after_space))
        {
            if (o.at > o.buf)
            {
                *clause = type;
                return true;
            }
            continue;               /* nothing said yet: no clause to end */
        }
        after_space = false;

        int script = script_of(c, text_has_kana);
        if (is_digit(c))
            read_number(&pos, &o);
        else if (latin_of(c, tmp, &upper))
            read_word(&pos, &o);
        else if (is_kana(c) || script >= 0)
        {
            /* a run of one script, Han and kana together as Japanese */
            bool han;
            const unsigned char *s = script_run(pos, script, text_has_kana,
                                                &han);
            if (script < 0 && !han)
            {
                char w[WORD_MAX];
                int n = romaji(&pos, w, WORD_MAX);
                put_word(&o, w, n);
                if (pos == token)
                    pos = next;     /* nothing romaji reads: pass it by */
                last_script = -1;
            }
            else
            {
                int name = han ? (text_has_kana ? SCRIPT_JAPANESE
                                                : SCRIPT_CHINESE)
                               : script;
                if (name != last_script)
                    put(&o, script_names[name]);
                last_script = name;
                pos = s;
            }
            if (o.full)
                goto overflow;
            continue;
        }
        else
        {
            pos = next;
            switch (c)
            {
            case '&':
                put(&o, "and");
                break;
            case '+':
                put(&o, *pos == ' ' ? "and" : "plus");
                break;
            case '%':
                put(&o, "percent");
                break;
            case '@':
                put(&o, "at");
                break;
            case '#':
                if (is_digit(*pos))
                    put(&o, "number");
                break;
            case '=':
                put(&o, "equals");
                break;
            }
        }
        if (o.full)
            goto overflow;
        last_script = -1;
        continue;

overflow:
        /* this token does not fit: end the clause before it */
        if (mark == o.buf)
        {
            o.full = false;     /* alone it is too long: keep what fitted */
            return true;
        }
        o.at = mark;
        *o.at = '\0';
        pos = token;
        return true;
    }
    return o.at > o.buf;
}
