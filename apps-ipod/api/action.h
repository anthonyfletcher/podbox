/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Boundary shim. firmware/backlight.c includes "action.h" by bare name.
 * This keeps that name resolvable from the include path no matter where
 * the real header lives inside apps/ -- currently input/action.h -- so
 * apps/ can be reorganised without editing files outside it. Update the
 * path below when the real header moves.
 ****************************************************************************/
#include "../input/action.h"
