/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Boundary shim. firmware/powermgmt.c includes "splash.h" by bare name. It
 * used to resolve because tools/configure put apps/gui on the include
 * path; that is no longer so, which makes this stub load-bearing rather
 * than documentary. The real header is widgets/splash.h.
 ****************************************************************************/
#include "../widgets/splash.h"
