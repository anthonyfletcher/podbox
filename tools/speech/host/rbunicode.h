/* What normalise.c needs of firmware/include/rbunicode.h, for the host
 * build of tools/speech/text_render.c, which defines utf8decode(). */
#ifndef _RBUNICODE_H_
#define _RBUNICODE_H_

#include <stdint.h>

typedef uint32_t ucschar_t;

const unsigned char *utf8decode(const unsigned char *utf8, ucschar_t *ucs);

#endif
