/***************************************************************************
 * GNU General Public License (version 2 only)
 *
 * Host renderer for apps-ipod/speech/synth*.c: reads clauses of IPA and
 * writes a WAV, so the port can be compared with NV Speech Player itself.
 *
 * Input, one clause per line: <clause> TAB <pause> TAB <ipa>, where clause
 * is one of . , ? ! or - for none, and pause is ignored (synth_frames.c
 * derives it). This is what the reference renderer writes with NVSP_DUMP.
 *
 * Build on Linux, where -DSYNTH_LIBC_RAND reproduces the reference's noise:
 *   gcc -O2 -DSYNTH_LIBC_RAND -Iapps-ipod/speech -Ilib/fixedpoint \
 *       -o synth_render tools/speech/synth_render.c apps-ipod/speech/synth.c \
 *       apps-ipod/speech/synth_frames.c apps-ipod/speech/synth_phonemes.c \
 *       lib/fixedpoint/fixedpoint.c
 * Run: synth_render <clauses.txt> <out.wav> [speed] [rate]
 * where speed is in thousandths (750, the default, is the chosen voice's).
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth.h"
#include "synth_frames.h"

static FILE *out;
static long total;

static void put_le(unsigned long v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        fputc((v >> (8 * i)) & 0xff, out);
}

static void write_header(int rate)
{
    fseek(out, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, out);
    put_le(36 + total * 2, 4);
    fwrite("WAVEfmt ", 1, 8, out);
    put_le(16, 4);
    put_le(1, 2);               /* PCM */
    put_le(1, 2);               /* mono */
    put_le(rate, 4);
    put_le(rate * 2, 4);
    put_le(2, 2);
    put_le(16, 2);
    fwrite("data", 1, 4, out);
    put_le(total * 2, 4);
}

static int render(int count)
{
    int16_t buf[4096];
    int n = synth_generate(buf, count);
    for (int i = 0; i < n; i++)
        put_le((uint16_t)buf[i], 2);
    total += n;
    return n;
}

/* UTF-8 to code points; the IPA is all in the BMP. */
static int decode(const char *s, uint16_t *cp, int max)
{
    const unsigned char *u = (const unsigned char *)s;
    int n = 0;
    while (*u && n < max)
    {
        if (*u < 0x80)
            cp[n++] = *u++;
        else if ((*u & 0xe0) == 0xc0)
        {
            cp[n++] = ((u[0] & 0x1f) << 6) | (u[1] & 0x3f);
            u += 2;
        }
        else
        {
            cp[n++] = ((u[0] & 0x0f) << 12) | ((u[1] & 0x3f) << 6)
                      | (u[2] & 0x3f);
            u += 3;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s clauses.txt out.wav [speed] [rate]\n",
                argv[0]);
        return 1;
    }
    int speed = argc > 3 ? atoi(argv[3]) : 750;
    int rate = argc > 4 ? atoi(argv[4]) : 22050;
    FILE *in = fopen(argv[1], "r");
    out = fopen(argv[2], "wb");
    if (!in || !out)
    {
        perror("open");
        return 1;
    }
    write_header(rate);
    synth_init(rate);

    char line[4096];
    while (fgets(line, sizeof(line), in))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *pause = strchr(line, '\t');
        char *ipa = pause ? strchr(pause + 1, '\t') : NULL;
        if (!ipa)
            continue;
        uint16_t cp[1024];
        int len = decode(ipa + 1, cp, 1024);
        int clause = line[0] == '-' ? 0 : line[0];
        if (!synth_clause_begin(cp, len, clause, speed))
            fprintf(stderr, "clause too long, truncated: %s\n", ipa + 1);
        while (!synth_clause_feed())
            render(256);
    }
    while (render(4096) > 0)
        ;
    write_header(rate);
    fclose(out);
    fclose(in);
    printf("%s: %ld samples, %.1f s\n", argv[2], total, (double)total / rate);
    return 0;
}
