/***************************************************************************
 * was: apps/rbcodecplatform.h
 * GNU General Public License (version 2+)
 *
 * Platform hooks the shared rbcodec library expects from the application.
 ****************************************************************************/
#ifndef RBCODECPLATFORM_H_INCLUDED
#define RBCODECPLATFORM_H_INCLUDED

/* rbcodec also expects assert, the ctype predicates, the {TYPE}_{MIN,MAX}
 * limits, the mem and str function families, and snprintf. It does not include
 * <assert.h>, <ctype.h>, <limits.h>, <string.h> or <stdio.h> for them --
 * everything that includes this already has them, and pulling them in again
 * here only lengthens the build. Only <stdlib.h> is needed on its own account,
 * for abs/atoi/labs/rand.
 *
 * debugf and logf are likewise expected but come from debug.h and logf.h via
 * the includer. HAVE_CLIP_SAMPLE_16 is deliberately NOT defined: there is no
 * platform-optimised clip_sample_16 here, so rbcodec uses its own. */
#include <stdlib.h>

bool tdspeed_alloc_buffers(int32_t **buffers, const int *buf_s, int nbuf);
void tdspeed_free_buffers(int32_t **buffers, int nbuf);

#endif

