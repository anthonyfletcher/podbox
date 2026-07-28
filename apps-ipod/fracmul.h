/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Boundary shim. lib/rbcodec/codecs/spc.c includes "../fracmul.h", which
 * resolves against the include path rather than by name, so this must sit
 * at the apps/ root. The real header is system/fracmul.h.
 ****************************************************************************/
#include "system/fracmul.h"
