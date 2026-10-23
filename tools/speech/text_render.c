/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Host driver for the speech front end and synthesiser: text in, words,
 * IPA or a WAV out.
 *
 *   text_render --words <cmudict.bin> <text>
 *   text_render --ipa <cmudict.bin> <text>
 *   text_render <cmudict.bin> <text> <out.wav> [speed] [rate]
 *
 * --words prints each clause's words as normalise.c makes them; --ipa each
 * clause as the reference renderer's NVSP_DUMP lines (clause, pause, IPA),
 * to compare with them. speed is in thousandths, 750 by default.
 *
 * Build on Linux from the repository root:
 *   gcc -O2 -Itools/speech/host -Iapps-ipod -Iapps-ipod/speech \
 *       -Ilib/fixedpoint -Ifirmware/include -DSYNTH_HOST -o text_render \
 *       tools/speech/text_render.c apps-ipod/speech/normalise.c \
 *       apps-ipod/speech/lexicon.c apps-ipod/speech/synth.c \
 *       apps-ipod/speech/synth_frames.c apps-ipod/speech/synth_phonemes.c \
 *       lib/fixedpoint/fixedpoint.c
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rbunicode.h"
#include "lexicon.h"
#include "normalise.h"
#include "synth.h"
#include "synth_frames.h"

#define CHUNK_IPA 160   /* code points per synth_clause_begin() */

/* Rockbox's own is in firmware/common/unicode.c. */
const unsigned char *utf8decode(const unsigned char *utf8, ucschar_t *ucs)
{
    unsigned char c = *utf8++;
    int tail = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    ucschar_t code = c < 0x80 ? c : c & (0x3f >> tail);
    while (tail-- && (*utf8 & 0xc0) == 0x80)
        code = (code << 6) | (*utf8++ & 0x3f);
    *ucs = code;
    return utf8;
}

static void print_utf8(const uint16_t *cp, int n)
{
    for (int i = 0; i < n; i++)
    {
        uint16_t c = cp[i];
        if (c < 0x80)
            putchar(c);
        else if (c < 0x800)
        {
            putchar(0xc0 | (c >> 6));
            putchar(0x80 | (c & 0x3f));
        }
        else
        {
            putchar(0xe0 | (c >> 12));
            putchar(0x80 | ((c >> 6) & 0x3f));
            putchar(0x80 | (c & 0x3f));
        }
    }
}

static FILE *wav;
static long samples;

static void render(int count)
{
    int16_t buf[4096];
    int n = synth_generate(buf, count);
    fwrite(buf, 2, n, wav);
    samples += n;
}

static void put32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = v >> (8 * i);
}

int main(int argc, char **argv)
{
    int mode = 0;                   /* 0 render, 1 words, 2 ipa */
    if (argc > 1 && !strcmp(argv[1], "--words"))
        mode = 1;
    else if (argc > 1 && !strcmp(argv[1], "--ipa"))
        mode = 2;
    if (mode)
        argv++, argc--;
    if (argc < (mode ? 3 : 4))
    {
        fprintf(stderr, "usage: see the head of text_render.c\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f)
    {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *dict = malloc(size);
    if (fread(dict, 1, size, f) != (size_t)size || !lexicon_init(dict, size))
    {
        fprintf(stderr, "%s: not a speech dictionary\n", argv[1]);
        return 1;
    }
    fclose(f);

    int speed = argc > 4 ? atoi(argv[4]) : 750;
    int rate = argc > 5 ? atoi(argv[5]) : 22050;
    if (mode == 0)
    {
        wav = fopen(argv[3], "wb");
        if (!wav)
        {
            perror(argv[3]);
            return 1;
        }
        fseek(wav, 44, SEEK_SET);
        synth_init(rate);
    }

    char words[512];
    int clause;
    normalise_begin(argv[2]);
    while (normalise_next(words, sizeof(words), &clause))
    {
        if (mode == 1)
        {
            printf("%c\t%s\n", clause ? clause : '-', words);
            continue;
        }
        const char *w = words;
        while (*w)
        {
            uint16_t ipa[CHUNK_IPA];
            int len;
            w += lexicon_ipa(w, ipa, CHUNK_IPA, &len);
            int type = *w ? 0 : clause;
            if (mode == 2)
            {
                int pause = type == ',' ? 120 : type ? 150 : 100;
                printf("%c\t%d\t", type ? type : '-', pause);
                print_utf8(ipa, len);
                putchar('\n');
                continue;
            }
            synth_clause_begin(ipa, len, type, speed);
            while (!synth_clause_feed())
                render(256);
        }
    }

    if (mode == 0)
    {
        long before;
        do
        {
            before = samples;
            render(4096);
        }
        while (samples != before);
        unsigned char h[44];
        memcpy(h, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0", 24);
        put32(h + 24, rate);
        put32(h + 28, rate * 2);
        memcpy(h + 32, "\x02\0\x10\0data", 8);
        put32(h + 40, samples * 2);
        put32(h + 4, 36 + samples * 2);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
        fprintf(stderr, "%s: %.1f s\n", argv[3], (double)samples / rate);
    }
    return 0;
}
