/***************************************************************************
 * was: apps/core_asmdefs.c
 * GNU General Public License (version 2+)
 *
 * Emits struct offsets as assembler constants, generating core_asmdefs.h
 * for the ARM assembly routines to include.
 ****************************************************************************/
#include <config.h>
#include "draw/bmp.h"

/* To export a value for use in assembly files, define an int or unsigned here
 * named AD_<name> and include apps-ipod/system/core_asmdefs.h in the assembly file.
 * Identifiers without the AD_ prefix will be ignored, and can be used to
 * create instances of structs for finding offsets to individual members.
 */


/* Size of a pixel with 8-bit components. */
const int AD_pix8_size =
    sizeof(struct uint8_rgb);
