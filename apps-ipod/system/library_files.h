/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Where every data file the library keeps lives. Three folders under
 * library/, by what losing a file costs: user/ is irreplaceable or written by
 * the owner, cache/ can be rebuilt, and library/ itself holds the sound
 * analysis, which can be rebuilt but takes hours. Logs are apart, in logs/.
 * Tagcache's own files stay in /.rockbox.
 ****************************************************************************/

#ifndef _LIBRARY_FILES_H
#define _LIBRARY_FILES_H

#include <stdbool.h>
#include "rbpaths.h"

#define LIB_DIR             ROCKBOX_DIR "/library"
#define LIB_USER_DIR        LIB_DIR "/user"
#define LIB_CACHE_DIR       LIB_DIR "/cache"
#define LIB_ART_DIR         LIB_CACHE_DIR "/art"
#define LIB_LOGS_DIR        ROCKBOX_DIR "/logs"

#define LIB_SOUND_FILE      LIB_DIR "/sound.dat"
#define LIB_SOUND_PART      LIB_DIR "/sound.part"
#define LIB_SOUND_CAL_FILE  LIB_DIR "/sound_calibration.dat"

/* Tagcache names files relative to /.rockbox */
#define LIB_PLAYS_NAME      "library/user/plays.txt"
#define LIB_PLAYS_FILE      ROCKBOX_DIR "/" LIB_PLAYS_NAME
#define LIB_PLAYBACK_LOG    LIB_USER_DIR "/playback.log"
#define LIB_PLAYBACK_STEM   LIB_USER_DIR "/playback"
#define LIB_AUDIOBOOKS_FILE LIB_USER_DIR "/audiobooks.txt"
#define LIB_BADGES_FILE     LIB_USER_DIR "/report_badges.dat"
#define LIB_QUIZ_FILE       LIB_USER_DIR "/quiz_scores.txt"
#define LIB_SPIKE_FILE      LIB_USER_DIR "/spike_scores.txt"
#define LIB_KNOWN_ARTISTS_FILE LIB_USER_DIR "/known_artists.txt"
#define LIB_PLAYER_NAME_FILE   LIB_USER_DIR "/player_name.txt"

#define LIB_ALBUMS_FILE     LIB_CACHE_DIR "/albums.dat"
#define LIB_ALBUM_PLAYS_FILE LIB_CACHE_DIR "/album_plays.dat"
#define LIB_COVERS_FILE     LIB_CACHE_DIR "/covers.cfg"
#define LIB_REPORT_INDEX_FILE LIB_CACHE_DIR "/report_index.dat"
#define LIB_REPORT_MOVES_FILE LIB_CACHE_DIR "/report_moves.dat"
#define LIB_DOCUMENTS_FILE  LIB_CACHE_DIR "/documents.txt"
#define LIB_IMAGES_FILE     LIB_CACHE_DIR "/images.txt"
#define LIB_SPIKE_RUN_FILE  LIB_CACHE_DIR "/spike_run.txt"
#define LIB_COVERS_EMPTY_FILE LIB_ART_DIR "/covers_empty.pfraw"

#define LIB_TAGCACHE_LOG    LIB_LOGS_DIR "/tagcache.log"
#define LIB_ART_LOG         LIB_LOGS_DIR "/art.log"
#define LIB_USB_LOG         LIB_LOGS_DIR "/usb.log"
#define LIB_BUFFER_LOG      LIB_LOGS_DIR "/buffer_damage.log"
#define LIB_UPGRADE_LOG     LIB_LOGS_DIR "/upgrade.log"

/* The libfile magic of each file kept in one (database/libfile.h), and the
 * version of its records where the file's own module does not set one */
#define LIB_STAMPS_MAGIC    0x53545241u     /* "ARTS"; version: the format */
#define LIB_PLAYS_MAGIC     0x504c4241u     /* "ABLP" */
#define LIB_PLAYS_VERSION   1
#define LIB_BADGES_MAGIC    0x53474442u     /* "BDGS" */
#define LIB_BADGES_VERSION  1
#define LIB_MOVES_MAGIC     0x45564f4du     /* "MOVE" */
#define LIB_MOVES_VERSION   1

/* Whether this boot has files to move into the layout above */
bool library_files_need_upgrade(void);

/* Creates any folder above that is missing, and moves the files of an older
 * layout into it. At boot, before anything reads them; progress is told as
 * done of total steps. */
void library_files_init(void (*progress)(int done, int total));

#endif /* _LIBRARY_FILES_H */
