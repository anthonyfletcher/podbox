/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Boundary shim. lib/rbcodec/dsp includes "fracmul.h" by bare name; the
 * real header is system/fracmul.h. Note that spc.c does NOT come through
 * here -- it uses "../fracmul.h", which resolves against the include path
 * rather than by name, and lands on the copy at the apps/ root.
 ****************************************************************************/
#include "../system/fracmul.h"
