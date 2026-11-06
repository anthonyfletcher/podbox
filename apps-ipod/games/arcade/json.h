/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to json.c: JSON to a cart's values and back.
 ***************************************************************************/
#ifndef ARCADE_JSON_H
#define ARCADE_JSON_H

#include <stdint.h>

struct vm;
struct value;

/* text's len bytes as a value into out: 1, or 0 with the cart stopped and
 * a message naming what (the file) and the byte where it went wrong */
int json_read(struct vm *vm, const uint8_t *text, uint32_t len,
              const char *what, struct value *out);

/* v as JSON text, a new string, into out: 1; 2 when it would be longer
 * than most bytes (out unset); 0 with the cart stopped, for a value JSON
 * cannot hold (a function, an image) or lists inside themselves */
int json_write(struct vm *vm, struct value v, const char *what,
               uint32_t most, struct value *out);

#endif
