/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to eq_cop_check.c.
 ****************************************************************************/

#ifndef EQ_COP_CHECK_H
#define EQ_COP_CHECK_H

#include <stdbool.h>

/* Filter the same noise with the current EQ on each core, and say whether
 * the two agree to the bit and how long each took. */
bool eq_cop_check_screen(void);

#endif /* EQ_COP_CHECK_H */
