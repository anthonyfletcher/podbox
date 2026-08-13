/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to settings_tags.c: what a setting is about, and how much of the
 * tree shows it.
 ****************************************************************************/

/* Two kinds of tag in one word.
 *
 * The LEVEL tag says how much of the tree a setting appears in: nothing means
 * it is always shown, TAG_ADVANCED means it is hidden until the user asks for
 * everything.
 *
 * TOPIC tags say what a setting is about. They are what search matches and
 * what a cross-cutting menu gathers -- "Battery & Power" is every setting
 * carrying TAG_BATTERY, wherever in the tree it actually lives, so a setting
 * can be found under a heading no branch of the tree provides.
 *
 * A setting carries at most one level tag and any number of topics.
 *
 * The tags live in a table of their own (settings_tags.c) rather than in
 * settings_list.c: that file is 1900 lines of upstream-shaped macros and this
 * tree mirrors upstream, so a per-setting edit there is a merge conflict on
 * every rebase. The cost is that the key is a string rather than a compile-time
 * reference, which settings_tags_validate() exists to cover.
 */

#ifndef _SETTINGS_TAGS_H_
#define _SETTINGS_TAGS_H_

#include <stdbool.h>
#include <stdint.h>
#include "settings_list.h"

/* Level. */
#define TAG_ADVANCED    0x0001

/* Topics. Two bits spare; widen the word rather than crowd these. */
#define TAG_SOUND       0x0002
#define TAG_PLAYBACK    0x0004
#define TAG_PLAYLIST    0x0008
#define TAG_LIBRARY     0x0010
#define TAG_DATABASE    0x0020
#define TAG_ARTWORK     0x0040
#define TAG_APPEARANCE  0x0080
#define TAG_SCROLLING   0x0100
#define TAG_THEMEAUTHOR 0x0200   /* pixel geometry; nobody else needs it */
#define TAG_BATTERY     0x0400
#define TAG_SYSTEM      0x0800
#define TAG_USB         0x1000
#define TAG_VOICE       0x2000

/* The tag word for `setting`, or 0 if it carries none. An untagged setting is
 * deliberately harmless: no topic finds it and it shows at every level. */
uint16_t settings_tags_get(const struct settings_list *setting);

/* True if `query` matches this setting by any of the four routes: its menu
 * name, its cfg name, the name of a topic it carries, or one of its own search
 * words. Case-insensitive substring throughout.
 *
 * There is no index behind this. With around 400 settings and a caller that
 * only asks once the query has settled, a linear pass is cheaper than anything
 * that would have to be kept in step. */
bool settings_tags_match(const struct settings_list *setting,
                         const char *query);

/* The English name of a single topic tag, for a menu title or a search hit --
 * NULL if `tag` is not exactly one topic. Not translated: the tags are a
 * search vocabulary, not screen furniture. A tag-built menu wants a real
 * lang_id and should carry one here when the first is built. */
const char *settings_tag_name(uint16_t tag);

/* How many rows of the tag table name no setting on THIS target, logging each.
 * For a debug screen.
 *
 * Not a pass/fail count. Some settings are compiled in per target -- the
 * backlight fades and the USB DAC are iPod Video only -- so their rows
 * legitimately resolve to nothing on the other one. What the count is good for
 * is a typo or a setting renamed out from under the table, which shows up as a
 * row that resolves on neither. */
int settings_tags_validate(void);

#endif /* _SETTINGS_TAGS_H_ */
