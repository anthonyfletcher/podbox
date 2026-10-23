/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Words to IPA for synth_frames.c. A word is looked up in the CMU
 * Pronouncing Dictionary (cmudict.bin, written by tools/speech/
 * gen_lexicon.py); one it lacks is pronounced by the letter-to-sound rules
 * of NRL Report 7948, "Automatic Translation of English Text to Phonetics by
 * Means of Letter-to-Sound Rules" (Elovitz, Johnson, McHugh and Shore, Naval
 * Research Laboratory, 1976), in John A. Wasser's public-domain
 * transcription of 1985.
 *
 * Phones are ARPABET, coded index * 4 + stress as gen_lexicon.py writes
 * them. The IPA is what synth_phonemes.c names, with a stress mark before a
 * stressed vowel. The dictionary is American, so its words are given a
 * British r and short o. The rules give no stress, so their words take it
 * on the first full vowel.
 *
 * Parts, in order:
 *   - phones and their IPA
 *   - the dictionary
 *   - the letter-to-sound rules and their matcher
 *   - a word's phones
 *   - the public interface
 ****************************************************************************/

#include <string.h>

#include "lexicon.h"

#define MAX_PHONES 64           /* per word */
#define MAX_WORD 64

/** Phones and their IPA **/

enum arpabet
{
    AA, AE, AH, AO, AW, AY, B, CH, D, DH, EH, ER, EY, F, G, HH, IH, IY, JH,
    K, L, M, N, NG, OW, OY, P, R, S, SH, T, TH, UH, UW, V, W, Y, Z, ZH,
    ER_NO_R,            /* ER where no vowel follows: the r goes */
    LOT,                /* AA or AO as the British short o of got */
};

#define CONSONANT 3             /* the stress of a consonant's code */
#define CODE(p, stress) ((p) * 4 + (stress))
#define PHONE(c) ((c) >> 2)
#define STRESS(c) ((c) & 3)

#define TIE 0x0361
#define LONG 0x02d0

static const uint16_t phone_ipa[][4] =
{
    [AA] = { 0x0251, LONG }, [AE] = { 0x00e6 }, [AH] = { 0x028c },
    [AO] = { 0x0254, LONG }, [AW] = { 'a', TIE, 0x028a },
    [AY] = { 0x0251, TIE, 0x026a }, [B] = { 'b' }, [CH] = { 't', TIE, 0x0283 },
    [D] = { 'd' }, [DH] = { 0x00f0 }, [EH] = { 0x025b },
    [ER] = { 0x025c, LONG, 0x0279 }, [EY] = { 'e', TIE, 'i' }, [F] = { 'f' },
    [G] = { 0x0261 }, [HH] = { 'h' }, [IH] = { 0x026a }, [IY] = { 'i', LONG },
    [JH] = { 'd', TIE, 0x0292 }, [K] = { 'k' }, [L] = { 'l' }, [M] = { 'm' },
    [N] = { 'n' }, [NG] = { 0x014b }, [OW] = { 'o', TIE, 'u' },
    [OY] = { 0x0254, TIE, 0x026a }, [P] = { 'p' }, [R] = { 0x0279 },
    [S] = { 's' }, [SH] = { 0x0283 }, [T] = { 't' }, [TH] = { 0x03b8 },
    [UH] = { 0x028a }, [UW] = { 'u', LONG }, [V] = { 'v' }, [W] = { 'w' },
    [Y] = { 'j' }, [Z] = { 'z' }, [ZH] = { 0x0292 },
    [ER_NO_R] = { 0x025c, LONG },
    [LOT] = { 0x0252 },
};

/* Unstressed, three vowels reduce. */
static const uint16_t schwa[] = { 0x0259 };
static const uint16_t schwa_r[] = { 0x0259, 0x0279 };
static const uint16_t short_i[] = { 'i' };

/* Append one phone's IPA at ipa[n], up to max. Returns the new n, or -1 if
 * it does not fit. */
static int put_phone(uint8_t code, uint16_t *ipa, int n, int max)
{
    int phone = PHONE(code), stress = STRESS(code);
    const uint16_t *s = phone_ipa[phone];
    int len = 0;
    while (len < 4 && s[len])
        len++;
    if (stress == 0)
    {
        if (phone == AH || phone == ER_NO_R)
            s = schwa, len = 1;
        else if (phone == ER)
            s = schwa_r, len = 2;
        else if (phone == IY)
            s = short_i, len = 1;
    }
    bool mark = stress == 1 || stress == 2;
    if (n + mark + len > max)
        return -1;
    if (mark)
        ipa[n++] = stress == 1 ? 0x02c8 : 0x02cc;
    for (int i = 0; i < len; i++)
        ipa[n++] = s[i];
    return n;
}

/** The dictionary **/

static const uint8_t *dict;
static size_t dict_size;
static uint32_t dict_blocks;

static uint32_t read32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint8_t *block_start(uint32_t i)
{
    uint32_t offset = read32(dict + 16 + 4 * i);
    return offset < dict_size ? dict + offset : NULL;
}

/* Compare word with the first word of block i, as strcmp. */
static int compare_first(uint32_t i, const char *word, int len)
{
    const uint8_t *p = block_start(i);
    if (!p || p + 2 + p[1] > dict + dict_size)
        return 1;
    int n = p[1] < len ? p[1] : len;
    int c = memcmp(p + 2, word, n);
    return c ? c : p[1] - len;
}

/* The phones of word, or -1 if the dictionary lacks it. */
static int dict_lookup(const char *word, int len, uint8_t *phones)
{
    if (!dict || len >= 256)
        return -1;

    /* the last block whose first word is not after this one */
    uint32_t lo = 0, hi = dict_blocks;
    while (hi - lo > 1)
    {
        uint32_t mid = (lo + hi) / 2;
        if (compare_first(mid, word, len) <= 0)
            lo = mid;
        else
            hi = mid;
    }

    const uint8_t *p = block_start(lo);
    const uint8_t *end = lo + 1 < dict_blocks ? block_start(lo + 1)
                                              : dict + dict_size;
    char current[256];
    while (p && end && p + 2 < end)
    {
        int prefix = p[0], suffix = p[1];
        if (prefix + suffix > 255 || p + 3 + suffix > end)
            break;
        memcpy(current + prefix, p + 2, suffix);
        int clen = prefix + suffix;
        int count = p[2 + suffix];
        const uint8_t *ph = p + 3 + suffix;
        if (ph + count > end)
            break;
        if (clen == len && !memcmp(current, word, len))
        {
            int n = count < MAX_PHONES ? count : MAX_PHONES;
            memcpy(phones, ph, n);
            return n;
        }
        p = ph + count;
    }
    return -1;
}

/** Letter-to-sound rules **/

/* Each rule is a left context, the letters it matches, a right context and
 * the phones they become. In a context:
 *   #  one or more vowels          :  zero or more consonants
 *   ^  one consonant               .  one of B D V G J L M N R W Z
 *   +  one of E I Y                %  one of ER E ES ED ING ELY (right only)
 *   ' ' the edge of the word
 * The phones are ARPABET, two capitals for a vowel and for CH DH NG SH TH
 * WH ZH, and a lower-case letter for the other consonants; AX is a reduced
 * vowel. The first rule for a letter that matches is used. */
struct nrl_rule
{
    const char *left, *match, *right, *out;
};

#define ANY ""
#define EDGE " "

static const struct nrl_rule punct_rules[] =
{
    { ANY, "-", ANY, "" },
    { ".", "'S", ANY, "z" },
    { "#:.E", "'S", ANY, "z" },
    { "#", "'S", ANY, "z" },
    { ANY, "'", ANY, "" },
    { ANY, ".", ANY, "" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule a_rules[] =
{
    { ANY, "A", EDGE, "AX" },
    { EDGE, "ARE", EDGE, "AAr" },
    { EDGE, "AR", "O", "AXr" },
    { ANY, "AR", "#", "EHr" },
    { "^", "AS", "#", "EYs" },
    { ANY, "A", "WA", "AX" },
    { ANY, "AW", ANY, "AO" },
    { " :", "ANY", ANY, "EHnIY" },
    { ANY, "A", "^+#", "EY" },
    { "#:", "ALLY", ANY, "AXlIY" },
    { EDGE, "AL", "#", "AXl" },
    { ANY, "AGAIN", ANY, "AXgEHn" },
    { "#:", "AG", "E", "IHj" },
    { ANY, "A", "^+:#", "AE" },
    { " :", "A", "^+ ", "EY" },
    { ANY, "A", "^%", "EY" },
    { EDGE, "ARR", ANY, "AXr" },
    { ANY, "ARR", ANY, "AEr" },
    { " :", "AR", EDGE, "AAr" },
    { ANY, "AR", EDGE, "ER" },
    { ANY, "AR", ANY, "AAr" },
    { ANY, "AIR", ANY, "EHr" },
    { ANY, "AI", ANY, "EY" },
    { ANY, "AY", ANY, "EY" },
    { ANY, "AU", ANY, "AO" },
    { "#:", "AL", EDGE, "AXl" },
    { "#:", "ALS", EDGE, "AXlz" },
    { ANY, "ALK", ANY, "AOk" },
    { ANY, "AL", "^", "AOl" },
    { " :", "ABLE", ANY, "EYbAXl" },
    { ANY, "ABLE", ANY, "AXbAXl" },
    { ANY, "ANG", "+", "EYnj" },
    { ANY, "A", ANY, "AE" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule b_rules[] =
{
    { EDGE, "BE", "^#", "bIH" },
    { ANY, "BEING", ANY, "bIYIHNG" },
    { EDGE, "BOTH", EDGE, "bOWTH" },
    { EDGE, "BUS", "#", "bIHz" },
    { ANY, "BUIL", ANY, "bIHl" },
    { ANY, "B", ANY, "b" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule c_rules[] =
{
    { EDGE, "CH", "^", "k" },
    { "^E", "CH", ANY, "k" },
    { ANY, "CH", ANY, "CH" },
    { " S", "CI", "#", "sAY" },
    { ANY, "CI", "A", "SH" },
    { ANY, "CI", "O", "SH" },
    { ANY, "CI", "EN", "SH" },
    { ANY, "C", "+", "s" },
    { ANY, "CK", ANY, "k" },
    { ANY, "COM", "%", "kAHm" },
    { ANY, "C", ANY, "k" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule d_rules[] =
{
    { "#:", "DED", EDGE, "dIHd" },
    { ".E", "D", EDGE, "d" },
    { "#:^E", "D", EDGE, "t" },
    { EDGE, "DE", "^#", "dIH" },
    { EDGE, "DO", EDGE, "dUW" },
    { EDGE, "DOES", ANY, "dAHz" },
    { EDGE, "DOING", ANY, "dUWIHNG" },
    { EDGE, "DOW", ANY, "dAW" },
    { ANY, "DU", "A", "jUW" },
    { ANY, "D", ANY, "d" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule e_rules[] =
{
    { "#:", "E", EDGE, "" },
    { "':^", "E", EDGE, "" },
    { " :", "E", EDGE, "IY" },
    { "#", "ED", EDGE, "d" },
    { "#:", "E", "D ", "" },
    { ANY, "EV", "ER", "EHv" },
    { ANY, "E", "^%", "IY" },
    { ANY, "ERI", "#", "IYrIY" },
    { ANY, "ERI", ANY, "EHrIH" },
    { "#:", "ER", "#", "ER" },
    { ANY, "ER", "#", "EHr" },
    { ANY, "ER", ANY, "ER" },
    { EDGE, "EVEN", ANY, "IYvEHn" },
    { "#:", "E", "W", "" },
    { "T", "EW", ANY, "UW" },
    { "S", "EW", ANY, "UW" },
    { "R", "EW", ANY, "UW" },
    { "D", "EW", ANY, "UW" },
    { "L", "EW", ANY, "UW" },
    { "Z", "EW", ANY, "UW" },
    { "N", "EW", ANY, "UW" },
    { "J", "EW", ANY, "UW" },
    { "TH", "EW", ANY, "UW" },
    { "CH", "EW", ANY, "UW" },
    { "SH", "EW", ANY, "UW" },
    { ANY, "EW", ANY, "yUW" },
    { ANY, "E", "O", "IY" },
    { "#:S", "ES", EDGE, "IHz" },
    { "#:C", "ES", EDGE, "IHz" },
    { "#:G", "ES", EDGE, "IHz" },
    { "#:Z", "ES", EDGE, "IHz" },
    { "#:X", "ES", EDGE, "IHz" },
    { "#:J", "ES", EDGE, "IHz" },
    { "#:CH", "ES", EDGE, "IHz" },
    { "#:SH", "ES", EDGE, "IHz" },
    { "#:", "E", "S ", "" },
    { "#:", "ELY", EDGE, "lIY" },
    { "#:", "EMENT", ANY, "mEHnt" },
    { ANY, "EFUL", ANY, "fUHl" },
    { ANY, "EE", ANY, "IY" },
    { ANY, "EARN", ANY, "ERn" },
    { EDGE, "EAR", "^", "ER" },
    { ANY, "EAD", ANY, "EHd" },
    { "#:", "EA", EDGE, "IYAX" },
    { ANY, "EA", "SU", "EH" },
    { ANY, "EA", ANY, "IY" },
    { ANY, "EIGH", ANY, "EY" },
    { ANY, "EI", ANY, "IY" },
    { EDGE, "EYE", ANY, "AY" },
    { ANY, "EY", ANY, "IY" },
    { ANY, "EU", ANY, "yUW" },
    { ANY, "E", ANY, "EH" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule f_rules[] =
{
    { ANY, "FUL", ANY, "fUHl" },
    { ANY, "F", ANY, "f" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule g_rules[] =
{
    { ANY, "GIV", ANY, "gIHv" },
    { EDGE, "G", "I^", "g" },
    { ANY, "GE", "T", "gEH" },
    { "SU", "GGES", ANY, "gjEHs" },
    { ANY, "GG", ANY, "g" },
    { " B#", "G", ANY, "g" },
    { ANY, "G", "+", "j" },
    { ANY, "GREAT", ANY, "grEYt" },
    { "#", "GH", ANY, "" },
    { ANY, "G", ANY, "g" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule h_rules[] =
{
    { EDGE, "HAV", ANY, "hAEv" },
    { EDGE, "HERE", ANY, "hIYr" },
    { EDGE, "HOUR", ANY, "AWER" },
    { ANY, "HOW", ANY, "hAW" },
    { ANY, "H", "#", "h" },
    { ANY, "H", ANY, "" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule i_rules[] =
{
    { EDGE, "IN", ANY, "IHn" },
    { EDGE, "I", EDGE, "AY" },
    { ANY, "IN", "D", "AYn" },
    { ANY, "IER", ANY, "IYER" },
    { "#:R", "IED", ANY, "IYd" },
    { ANY, "IED", EDGE, "AYd" },
    { ANY, "IEN", ANY, "IYEHn" },
    { ANY, "IE", "T", "AYEH" },
    { " :", "I", "%", "AY" },
    { ANY, "I", "%", "IY" },
    { ANY, "IE", ANY, "IY" },
    { ANY, "I", "^+:#", "IH" },
    { ANY, "IR", "#", "AYr" },
    { ANY, "IZ", "%", "AYz" },
    { ANY, "IS", "%", "AYz" },
    { ANY, "I", "D%", "AY" },
    { "+^", "I", "^+", "IH" },
    { ANY, "I", "T%", "AY" },
    { "#:^", "I", "^+", "IH" },
    { ANY, "I", "^+", "AY" },
    { ANY, "IR", ANY, "ER" },
    { ANY, "IGH", ANY, "AY" },
    { ANY, "ILD", ANY, "AYld" },
    { ANY, "IGN", EDGE, "AYn" },
    { ANY, "IGN", "^", "AYn" },
    { ANY, "IGN", "%", "AYn" },
    { ANY, "IQUE", ANY, "IYk" },
    { ANY, "I", ANY, "IH" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule j_rules[] =
{
    { ANY, "J", ANY, "j" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule k_rules[] =
{
    { EDGE, "K", "N", "" },
    { ANY, "K", ANY, "k" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule l_rules[] =
{
    { ANY, "LO", "C#", "lOW" },
    { "L", "L", ANY, "" },
    { "#:^", "L", "%", "AXl" },
    { ANY, "LEAD", ANY, "lIYd" },
    { ANY, "L", ANY, "l" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule m_rules[] =
{
    { ANY, "MOV", ANY, "mUWv" },
    { ANY, "M", ANY, "m" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule n_rules[] =
{
    { "E", "NG", "+", "nj" },
    { ANY, "NG", "R", "NGg" },
    { ANY, "NG", "#", "NGg" },
    { ANY, "NGL", "%", "NGgAXl" },
    { ANY, "NG", ANY, "NG" },
    { ANY, "NK", ANY, "NGk" },
    { EDGE, "NOW", EDGE, "nAW" },
    { ANY, "N", ANY, "n" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule o_rules[] =
{
    { ANY, "OF", EDGE, "AXv" },
    { ANY, "OROUGH", ANY, "EROW" },
    { "#:", "OR", EDGE, "ER" },
    { "#:", "ORS", EDGE, "ERz" },
    { ANY, "OR", ANY, "AOr" },
    { EDGE, "ONE", ANY, "wAHn" },
    { ANY, "OW", ANY, "OW" },
    { EDGE, "OVER", ANY, "OWvER" },
    { ANY, "OV", ANY, "AHv" },
    { ANY, "O", "^%", "OW" },
    { ANY, "O", "^EN", "OW" },
    { ANY, "O", "^I#", "OW" },
    { ANY, "OL", "D", "OWl" },
    { ANY, "OUGHT", ANY, "AOt" },
    { ANY, "OUGH", ANY, "AHf" },
    { EDGE, "OU", ANY, "AW" },
    { "H", "OU", "S#", "AW" },
    { ANY, "OUS", ANY, "AXs" },
    { ANY, "OUR", ANY, "AOr" },
    { ANY, "OULD", ANY, "UHd" },
    { "^", "OU", "^L", "AH" },
    { ANY, "OUP", ANY, "UWp" },
    { ANY, "OU", ANY, "AW" },
    { ANY, "OY", ANY, "OY" },
    { ANY, "OING", ANY, "OWIHNG" },
    { ANY, "OI", ANY, "OY" },
    { ANY, "OOR", ANY, "AOr" },
    { ANY, "OOK", ANY, "UHk" },
    { ANY, "OOD", ANY, "UHd" },
    { ANY, "OO", ANY, "UW" },
    { ANY, "O", "E", "OW" },
    { ANY, "O", EDGE, "OW" },
    { ANY, "OA", ANY, "OW" },
    { EDGE, "ONLY", ANY, "OWnlIY" },
    { EDGE, "ONCE", ANY, "wAHns" },
    { ANY, "ON'T", ANY, "OWnt" },
    { "C", "O", "N", "AA" },
    { ANY, "O", "NG", "AO" },
    { " :^", "O", "N", "AH" },
    { "I", "ON", ANY, "AXn" },
    { "#:", "ON", EDGE, "AXn" },
    { "#^", "ON", ANY, "AXn" },
    { ANY, "O", "ST ", "OW" },
    { ANY, "OF", "^", "AOf" },
    { ANY, "OTHER", ANY, "AHDHER" },
    { ANY, "OSS", EDGE, "AOs" },
    { "#:^", "OM", ANY, "AHm" },
    { ANY, "O", ANY, "AA" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule p_rules[] =
{
    { ANY, "PH", ANY, "f" },
    { ANY, "PEOP", ANY, "pIYp" },
    { ANY, "POW", ANY, "pAW" },
    { ANY, "PUT", EDGE, "pUHt" },
    { ANY, "P", ANY, "p" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule q_rules[] =
{
    { ANY, "QUAR", ANY, "kwAOr" },
    { ANY, "QU", ANY, "kw" },
    { ANY, "Q", ANY, "k" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule r_rules[] =
{
    { EDGE, "RE", "^#", "rIY" },
    { ANY, "R", ANY, "r" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule s_rules[] =
{
    { ANY, "SH", ANY, "SH" },
    { "#", "SION", ANY, "ZHAXn" },
    { ANY, "SOME", ANY, "sAHm" },
    { "#", "SUR", "#", "ZHER" },
    { ANY, "SUR", "#", "SHER" },
    { "#", "SU", "#", "ZHUW" },
    { "#", "SSU", "#", "SHUW" },
    { "#", "SED", EDGE, "zd" },
    { "#", "S", "#", "z" },
    { ANY, "SAID", ANY, "sEHd" },
    { "^", "SION", ANY, "SHAXn" },
    { ANY, "S", "S", "" },
    { ".", "S", EDGE, "z" },
    { "#:.E", "S", EDGE, "z" },
    { "#:^##", "S", EDGE, "z" },
    { "#:^#", "S", EDGE, "s" },
    { "U", "S", EDGE, "s" },
    { " :#", "S", EDGE, "z" },
    { EDGE, "SCH", ANY, "sk" },
    { ANY, "S", "C+", "" },
    { "#", "SM", ANY, "zm" },
    { "#", "SN", "'", "zAXn" },
    { ANY, "S", ANY, "s" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule t_rules[] =
{
    { EDGE, "THE", EDGE, "DHAX" },
    { ANY, "TO", EDGE, "tUW" },
    { ANY, "THAT", EDGE, "DHAEt" },
    { EDGE, "THIS", EDGE, "DHIHs" },
    { EDGE, "THEY", ANY, "DHEY" },
    { EDGE, "THERE", ANY, "DHEHr" },
    { ANY, "THER", ANY, "DHER" },
    { ANY, "THEIR", ANY, "DHEHr" },
    { EDGE, "THAN", EDGE, "DHAEn" },
    { EDGE, "THEM", EDGE, "DHEHm" },
    { ANY, "THESE", EDGE, "DHIYz" },
    { EDGE, "THEN", ANY, "DHEHn" },
    { ANY, "THROUGH", ANY, "THrUW" },
    { ANY, "THOSE", ANY, "DHOWz" },
    { ANY, "THOUGH", EDGE, "DHOW" },
    { EDGE, "THUS", ANY, "DHAHs" },
    { ANY, "TH", ANY, "TH" },
    { "#:", "TED", EDGE, "tIHd" },
    { "S", "TI", "#N", "CH" },
    { ANY, "TI", "O", "SH" },
    { ANY, "TI", "A", "SH" },
    { ANY, "TIEN", ANY, "SHAXn" },
    { ANY, "TUR", "#", "CHER" },
    { ANY, "TU", "A", "CHUW" },
    { EDGE, "TWO", ANY, "tUW" },
    { ANY, "T", ANY, "t" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule u_rules[] =
{
    { EDGE, "UN", "I", "yUWn" },
    { EDGE, "UN", ANY, "AHn" },
    { EDGE, "UPON", ANY, "AXpAOn" },
    { "T", "UR", "#", "UHr" },
    { "S", "UR", "#", "UHr" },
    { "R", "UR", "#", "UHr" },
    { "D", "UR", "#", "UHr" },
    { "L", "UR", "#", "UHr" },
    { "Z", "UR", "#", "UHr" },
    { "N", "UR", "#", "UHr" },
    { "J", "UR", "#", "UHr" },
    { "TH", "UR", "#", "UHr" },
    { "CH", "UR", "#", "UHr" },
    { "SH", "UR", "#", "UHr" },
    { ANY, "UR", "#", "yUHr" },
    { ANY, "UR", ANY, "ER" },
    { ANY, "U", "^ ", "AH" },
    { ANY, "U", "^^", "AH" },
    { ANY, "UY", ANY, "AY" },
    { " G", "U", "#", "" },
    { "G", "U", "%", "" },
    { "G", "U", "#", "w" },
    { "#N", "U", ANY, "yUW" },
    { "T", "U", ANY, "UW" },
    { "S", "U", ANY, "UW" },
    { "R", "U", ANY, "UW" },
    { "D", "U", ANY, "UW" },
    { "L", "U", ANY, "UW" },
    { "Z", "U", ANY, "UW" },
    { "N", "U", ANY, "UW" },
    { "J", "U", ANY, "UW" },
    { "TH", "U", ANY, "UW" },
    { "CH", "U", ANY, "UW" },
    { "SH", "U", ANY, "UW" },
    { ANY, "U", ANY, "yUW" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule v_rules[] =
{
    { ANY, "VIEW", ANY, "vyUW" },
    { ANY, "V", ANY, "v" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule w_rules[] =
{
    { EDGE, "WERE", ANY, "wER" },
    { ANY, "WA", "S", "wAA" },
    { ANY, "WA", "T", "wAA" },
    { ANY, "WHERE", ANY, "WHEHr" },
    { ANY, "WHAT", ANY, "WHAAt" },
    { ANY, "WHOL", ANY, "hOWl" },
    { ANY, "WHO", ANY, "hUW" },
    { ANY, "WH", ANY, "WH" },
    { ANY, "WAR", ANY, "wAOr" },
    { ANY, "WOR", "^", "wER" },
    { ANY, "WR", ANY, "r" },
    { ANY, "W", ANY, "w" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule x_rules[] =
{
    { ANY, "X", ANY, "ks" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule y_rules[] =
{
    { ANY, "YOUNG", ANY, "yAHNG" },
    { EDGE, "YOU", ANY, "yUW" },
    { EDGE, "YES", ANY, "yEHs" },
    { EDGE, "Y", ANY, "y" },
    { "#:^", "Y", EDGE, "IY" },
    { "#:^", "Y", "I", "IY" },
    { " :", "Y", EDGE, "AY" },
    { " :", "Y", "#", "AY" },
    { " :", "Y", "^+:#", "IH" },
    { " :", "Y", "^#", "AY" },
    { ANY, "Y", ANY, "IH" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule z_rules[] =
{
    { ANY, "Z", ANY, "z" },
    { ANY, NULL, ANY, "" },
};

static const struct nrl_rule *const nrl_rules[27] =
{
    punct_rules, a_rules, b_rules, c_rules, d_rules, e_rules, f_rules,
    g_rules, h_rules, i_rules, j_rules, k_rules, l_rules, m_rules, n_rules,
    o_rules, p_rules, q_rules, r_rules, s_rules, t_rules, u_rules, v_rules,
    w_rules, x_rules, y_rules, z_rules,
};

static bool is_vowel(char c)
{
    return c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U';
}

static bool is_consonant(char c)
{
    return c >= 'A' && c <= 'Z' && !is_vowel(c);
}

static bool is_voiced(char c)
{
    return c && strchr("BDVGJLMNRWZ", c);
}

static bool is_front(char c)
{
    return c == 'E' || c == 'I' || c == 'Y';
}

/* Match pattern leftwards from text, the letter before the match. */
static bool left_match(const char *pattern, const char *text)
{
    for (int i = strlen(pattern) - 1; i >= 0; i--)
    {
        char p = pattern[i];
        if ((p >= 'A' && p <= 'Z') || p == '\'' || p == ' ')
        {
            if (p != *text--)
                return false;
            continue;
        }
        switch (p)
        {
        case '#':
            if (!is_vowel(*text--))
                return false;
            while (is_vowel(*text))
                text--;
            break;
        case ':':
            while (is_consonant(*text))
                text--;
            break;
        case '^':
            if (!is_consonant(*text--))
                return false;
            break;
        case '.':
            if (!is_voiced(*text--))
                return false;
            break;
        case '+':
            if (!is_front(*text--))
                return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

/* Match pattern rightwards from text, the letter after the match. */
static bool right_match(const char *pattern, const char *text)
{
    for (; *pattern; pattern++)
    {
        char p = *pattern;
        if ((p >= 'A' && p <= 'Z') || p == '\'' || p == ' ')
        {
            if (p != *text++)
                return false;
            continue;
        }
        switch (p)
        {
        case '#':
            if (!is_vowel(*text++))
                return false;
            while (is_vowel(*text))
                text++;
            break;
        case ':':
            while (is_consonant(*text))
                text++;
            break;
        case '^':
            if (!is_consonant(*text++))
                return false;
            break;
        case '.':
            if (!is_voiced(*text++))
                return false;
            break;
        case '+':
            if (!is_front(*text++))
                return false;
            break;
        case '%':           /* E, ER, ES, ED, ELY or ING */
            if (*text == 'E')
            {
                text++;
                if (text[0] == 'L' && text[1] == 'Y')
                    text += 2;
                else if (*text == 'R' || *text == 'S' || *text == 'D')
                    text++;
            }
            else if (text[0] == 'I' && text[1] == 'N' && text[2] == 'G')
                text += 3;
            else
                return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

/* Append a rule's phones; vowels get stress 0 for now, AX as AH with its
 * bit set in *reduced so that it is never the one stressed. */
static int put_rule_phones(const char *out, uint8_t *phones, int n,
                           uint64_t *reduced)
{
    static const char vowels[][3] =
        { "AA", "AE", "AH", "AO", "AW", "AY", "EH", "ER", "EY", "IH", "IY",
          "OW", "OY", "UH", "UW" };
    static const uint8_t vowel_phone[] =
        { AA, AE, AH, AO, AW, AY, EH, ER, EY, IH, IY, OW, OY, UH, UW };
    static const char pairs[][3] = { "CH", "DH", "NG", "SH", "TH", "WH", "ZH" };
    static const uint8_t pair_phone[] = { CH, DH, NG, SH, TH, W, ZH };
    static const char singles[] = "pbtdkgfvszmnlwyrjh";
    static const uint8_t single_phone[] =
        { P, B, T, D, K, G, F, V, S, Z, M, N, L, W, Y, R, JH, HH };

    while (*out && n < MAX_PHONES)
    {
        if (out[0] == 'A' && out[1] == 'X')
        {
            *reduced |= 1ull << n;
            phones[n++] = CODE(AH, 0);
            out += 2;
            continue;
        }
        bool found = false;
        for (unsigned i = 0; i < sizeof(vowels) / sizeof(*vowels); i++)
            if (out[0] == vowels[i][0] && out[1] == vowels[i][1])
            {
                phones[n++] = CODE(vowel_phone[i], 0);
                found = true;
                break;
            }
        for (unsigned i = 0; !found && i < sizeof(pairs) / sizeof(*pairs); i++)
            if (out[0] == pairs[i][0] && out[1] == pairs[i][1])
            {
                phones[n++] = CODE(pair_phone[i], CONSONANT);
                found = true;
            }
        if (found)
        {
            out += 2;
            continue;
        }
        const char *s = strchr(singles, *out);
        if (s && *out)
            phones[n++] = CODE(single_phone[s - singles], CONSONANT);
        out++;
    }
    return n;
}

/* The letter-to-sound rules' phones for a word, stressed on its first full
 * vowel. */
static int rule_phones(const char *word, int len, uint8_t *phones)
{
    char buf[MAX_WORD + 5] = "  ";
    if (len > MAX_WORD)
        len = MAX_WORD;
    for (int i = 0; i < len; i++)
        buf[2 + i] = word[i] >= 'a' && word[i] <= 'z' ? word[i] - 'a' + 'A'
                                                      : word[i];
    buf[2 + len] = ' ';
    buf[3 + len] = ' ';
    buf[4 + len] = '\0';

    int n = 0;
    int at = 2;
    uint64_t reduced = 0;
    while (at < 2 + len && n < MAX_PHONES)
    {
        char c = buf[at];
        const struct nrl_rule *r = nrl_rules[c >= 'A' && c <= 'Z'
                                             ? c - 'A' + 1 : 0];
        for (;; r++)
        {
            if (!r->match)
            {
                at++;           /* no rule: skip the character */
                break;
            }
            int mlen = strlen(r->match);
            if (strncmp(r->match, &buf[at], mlen))
                continue;
            if (!left_match(r->left, &buf[at - 1])
                || !right_match(r->right, &buf[at + mlen]))
                continue;
            n = put_rule_phones(r->out, phones, n, &reduced);
            at += mlen;
            break;
        }
    }

    /* the rules read each letter of a doubled consonant: say it once */
    int kept = 0;
    uint64_t kept_reduced = 0;
    for (int i = 0; i < n; i++)
    {
        if (kept > 0 && STRESS(phones[i]) == CONSONANT
            && phones[i] == phones[kept - 1])
            continue;
        if (reduced & (1ull << i))
            kept_reduced |= 1ull << kept;
        phones[kept++] = phones[i];
    }
    n = kept;
    reduced = kept_reduced;

    for (int i = 0; i < n; i++)
    {
        if (STRESS(phones[i]) == 0 && !(reduced & (1ull << i)))
        {
            phones[i] = CODE(PHONE(phones[i]), 1);
            break;
        }
    }
    return n;
}

/** A word's phones **/

static int word_phones(const char *word, int len, uint8_t *phones)
{
    int n = dict_lookup(word, len, phones);
    if (n >= 0)
        return n;

    /* a possessive the dictionary lacks: the word's own, then s or z */
    if (len > 2 && word[len - 2] == '\'' && word[len - 1] == 's')
    {
        n = dict_lookup(word, len - 2, phones);
        if (n > 0 && n < MAX_PHONES - 1)
        {
            int last = PHONE(phones[n - 1]);
            if (last == S || last == Z || last == SH || last == ZH
                || last == CH || last == JH)
            {
                phones[n++] = CODE(IH, 0);
                phones[n++] = CODE(Z, CONSONANT);
            }
            else if (last == P || last == T || last == K || last == F
                     || last == TH)
                phones[n++] = CODE(S, CONSONANT);
            else
                phones[n++] = CODE(Z, CONSONANT);
            return n;
        }
    }

    /* hyphened or dotted, and not in the dictionary as a whole: its parts,
     * each in its own right */
    if (memchr(word, '-', len) || memchr(word, '.', len))
    {
        n = 0;
        const char *p = word, *end = word + len;
        while (p < end)
        {
            const char *q = p;
            while (q < end && *q != '-' && *q != '.')
                q++;
            if (q > p)
            {
                uint8_t part[MAX_PHONES];
                int m = word_phones(p, q - p, part);
                if (m > MAX_PHONES - n)
                    m = MAX_PHONES - n;
                memcpy(phones + n, part, m);
                n += m;
            }
            p = q + 1;
        }
        return n;
    }

    /* a prefix on a word the dictionary has: remastered, unbroken */
    static const struct
    {
        const char *letters;
        int count;
        uint8_t phones[3];
    } prefixes[] =
    {
        { "re", 2, { CODE(R, CONSONANT), CODE(IY, 0) } },
        { "un", 2, { CODE(AH, 0), CODE(N, CONSONANT) } },
        { "dis", 3, { CODE(D, CONSONANT), CODE(IH, 0), CODE(S, CONSONANT) } },
        { "pre", 3, { CODE(P, CONSONANT), CODE(R, CONSONANT), CODE(IY, 0) } },
        { "mis", 3, { CODE(M, CONSONANT), CODE(IH, 0), CODE(S, CONSONANT) } },
        { "de", 2, { CODE(D, CONSONANT), CODE(IY, 0) } },
        { "over", 3, { CODE(OW, 2), CODE(V, CONSONANT), CODE(ER, 0) } },
        { "out", 2, { CODE(AW, 2), CODE(T, CONSONANT) } },
    };
    uint8_t rest[MAX_PHONES];
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++)
    {
        int plen = strlen(prefixes[i].letters);
        if (len - plen < 4 || memcmp(word, prefixes[i].letters, plen))
            continue;
        n = dict_lookup(word + plen, len - plen, rest);
        if (n < 0)
            continue;
        int pn = prefixes[i].count;
        if (n > MAX_PHONES - pn)
            n = MAX_PHONES - pn;
        memcpy(phones, prefixes[i].phones, pn);
        memcpy(phones + pn, rest, n);
        return pn + n;
    }

    /* two words the dictionary has, run together: brightside */
    for (int split = len - 3; split >= 3; split--)
    {
        int a = dict_lookup(word, split, phones);
        if (a < 0)
            continue;
        int b = dict_lookup(word + split, len - split, rest);
        if (b < 0)
            continue;
        if (b > MAX_PHONES - a)
            b = MAX_PHONES - a;
        memcpy(phones + a, rest, b);
        return a + b;
    }
    return rule_phones(word, len, phones);
}

/** Interface **/

bool lexicon_init(const void *data, size_t size)
{
    const uint8_t *d = data;
    dict = NULL;
    if (!d || size < 16 || memcmp(d, "PBLX", 4) || d[4] != 1 || d[5] != 0)
        return false;
    uint32_t blocks = read32(d + 8);
    if (blocks == 0 || 16 + 4 * (size_t)blocks > size)
        return false;
    dict = d;
    dict_size = size;
    dict_blocks = blocks;
    return true;
}

bool lexicon_has(const char *word, int len)
{
    uint8_t phones[MAX_PHONES];
    return dict_lookup(word, len, phones) >= 0;
}

/* The British r: kept only where a vowel follows, so car, over and forty
 * lose theirs and story keeps it. An American final r is heard as n. */
static int drop_r(uint8_t *phones, int count)
{
    int kept = 0;
    for (int i = 0; i < count; i++)
    {
        bool vowel_next = i + 1 < count
                          && STRESS(phones[i + 1]) != CONSONANT;
        if (PHONE(phones[i]) == R && !vowel_next)
            continue;
        if (PHONE(phones[i]) == ER && !vowel_next)
            phones[kept++] = CODE(ER_NO_R, STRESS(phones[i]));
        else
            phones[kept++] = phones[i];
    }
    return kept;
}

/* The British short o. AA takes it unless an r follows or the word is spelt
 * with an a alone (father, spa; but want, quality); AO takes it when spelt
 * with a plain o (dog, long; but thought, broad, door). */
static void british_o(const char *word, int len, uint8_t *phones, int count)
{
    bool o = false, plain_o = false, wa = false;
    for (int i = 0; i < len; i++)
    {
        char c = word[i] | 0x20;
        char next = i + 1 < len ? word[i + 1] | 0x20 : 0;
        if (c == 'o')
        {
            o = true;
            if (next != 'u' && next != 'a' && next != 'r')
                plain_o = true;
        }
        if (c == 'a' && i > 0 && ((word[i - 1] | 0x20) == 'w'
                                  || (word[i - 1] | 0x20) == 'u'))
            wa = true;
    }
    for (int i = 0; i < count; i++)
    {
        int phone = PHONE(phones[i]);
        bool r_next = i + 1 < count && PHONE(phones[i + 1]) == R;
        if (!r_next && ((phone == AA && (o || wa))
                        || (phone == AO && plain_o)))
            phones[i] = CODE(LOT, STRESS(phones[i]));
    }
}

size_t lexicon_ipa(const char *words, uint16_t *ipa, int max, int *len)
{
    const char *p = words;
    int n = 0;
    *len = 0;
    while (*p)
    {
        while (*p == ' ')
            p++;
        const char *end = p;
        while (*end && *end != ' ')
            end++;
        if (end == p)
            break;

        uint8_t phones[MAX_PHONES];
        int count = word_phones(p, end - p, phones);
        british_o(p, end - p, phones, count);
        count = drop_r(phones, count);
        int m = n;
        if (m > 0)
        {
            if (m >= max)
                break;
            ipa[m++] = ' ';
        }
        for (int i = 0; i < count && m >= 0; i++)
            m = put_phone(phones[i], ipa, m, max);
        if (m < 0)
        {
            if (n > 0)
                break;          /* the word starts the next call */
            m = max;            /* a word too long to fit at all: cut it */
        }
        n = m;
        p = end;
    }
    *len = n;
    return p - words;
}
