/* CRC recovery retains complete and partial trailing frames. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "podlink.h"

int main(void)
{
    const unsigned int chunks[] = {1, 7, 64, 4096};
    const unsigned int lengths[] = {32, 47};
    for (unsigned int l = 0; l < 2; ++l)
    {
        unsigned char stream[128];
        podlink_encode(stream, PODLINK_TEXT, 1, NULL, 0);
        stream[12] = lengths[l];
        unsigned int size = 16;
        size += podlink_encode(stream + size, PODLINK_TEXT, 2, "first", 5);
        size += podlink_encode(stream + size, PODLINK_TEXT, 3, "second", 6);
        for (unsigned int c = 0; c < 4; ++c)
        {
            struct podlink_parser parser = {0};
            struct podlink_message m;
            unsigned int pos = 0, got = 0;
            while (pos < size)
            {
                unsigned int chunk = chunks[c];
                if (chunk > size - pos)
                    chunk = size - pos;
                unsigned int used = 0;
                for (unsigned int calls = 0;; ++calls)
                {
                    assert(calls < chunk + 100);
                    unsigned int consumed;
                    int rc = podlink_decode_bytes(&parser, stream + pos + used,
                                                  chunk - used, &consumed, &m);
                    used += consumed;
                    if (rc == 1)
                    {
                        ++got;
                        assert(m.seq == got + 1);
                        const char *expected = got == 1 ? "first" : "second";
                        assert(m.length == strlen(expected));
                        assert(!memcmp(m.data, expected, m.length));
                    }
                    if (!rc && used == chunk)
                        break;
                }
                pos += chunk;
            }
            assert(got == 2);
        }
    }
    puts("PASS: CRC recovery retains trailing frames and drains without input");
    return 0;
}
