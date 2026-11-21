/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to codec_cop_check.c.
 ****************************************************************************/

#ifndef CODEC_COP_CHECK_H
#define CODEC_COP_CHECK_H

#include <stdbool.h>

/* Decode each file in the playing track's folder on the CPU and on the COP,
 * and say whether the two gave the same samples. */
bool codec_cop_check_screen(void);

#endif /* CODEC_COP_CHECK_H */
