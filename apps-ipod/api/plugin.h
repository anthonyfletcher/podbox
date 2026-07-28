/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Vestigial boundary stub. lib/rbcodec/metadata/hes.c has a leftover
 * include of "plugin.h" from when this fork had a plugin system; it needs
 * nothing from the header, and the build succeeds with it empty. Unlike
 * the other stubs here this one forwards to nothing, because apps/plugin.h
 * was deleted -- no file inside apps/ included it. To finish the job:
 * delete the include at lib/rbcodec/metadata/hes.c:12, then delete this
 * file.
 ****************************************************************************/

#ifndef _PLUGIN_H_
#define _PLUGIN_H_

#endif /* _PLUGIN_H_ */
