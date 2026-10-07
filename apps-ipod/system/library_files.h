/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Where every data file the library keeps lives. Three folders under
 * library/, by what losing a file costs: user/ is irreplaceable or written by
 * the owner, cache/ can be rebuilt, and library/ itself holds the database
 * and the sound analysis, which hold play data or take hours to rebuild. Logs
 * are apart, in logs/.
 *
 * A .txt is the owner's to edit; every .dat is the player's own, a libfile
 * (database/libfile.h) whatever is inside it.
 ****************************************************************************/

#ifndef _LIBRARY_FILES_H
#define _LIBRARY_FILES_H

#include <stdbool.h>
#include "rbpaths.h"

#define LIB_DIR             ROCKBOX_DIR "/library"
#define LIB_DB_DIR          LIB_DIR "/database"
#define LIB_USER_DIR        LIB_DIR "/user"
#define LIB_CACHE_DIR       LIB_DIR "/cache"
#define LIB_ART_DIR         LIB_CACHE_DIR "/art"
#define LIB_LOGS_DIR        ROCKBOX_DIR "/logs"

#define LIB_FORMAT_FILE     LIB_DIR "/format.dat"
#define LIB_SOUND_FILE      LIB_DIR "/sound.dat"
#define LIB_SOUND_PART      LIB_DIR "/sound.part"
#define LIB_SOUND_CAL_FILE  LIB_DIR "/sound_calibration.dat"

#define LIB_PLAYS_FILE      LIB_USER_DIR "/plays.dat"
#define LIB_PLAYBACK_LOG    LIB_USER_DIR "/playback.log"
#define LIB_PLAYBACK_STEM   LIB_USER_DIR "/playback"
#define LIB_AUDIOBOOKS_FILE LIB_USER_DIR "/audiobooks.dat"
#define LIB_BADGES_FILE     LIB_USER_DIR "/report_badges.dat"
#define LIB_QUIZ_FILE       LIB_USER_DIR "/quiz_scores.dat"
#define LIB_SPIKE_FILE      LIB_USER_DIR "/spike_scores.dat"
#define LIB_KNOWN_ARTISTS_FILE LIB_USER_DIR "/known_artists.txt"
#define LIB_PLAYER_NAME_FILE   LIB_USER_DIR "/player_name.txt"

/* Export and Import Modifications: Rockbox's own name and place, which its
 * Import reads, so a copy goes to other firmware and comes back */
#define LIB_EXPORT_FILE     ROCKBOX_DIR "/database_changelog.txt"

#define LIB_ALBUMS_FILE     LIB_CACHE_DIR "/albums.dat"
#define LIB_ALBUM_PLAYS_FILE LIB_CACHE_DIR "/album_plays.dat"
#define LIB_COVERS_FILE     LIB_CACHE_DIR "/covers.cfg"
#define LIB_REPORT_INDEX_FILE LIB_CACHE_DIR "/report_index.dat"
#define LIB_REPORT_MOVES_FILE LIB_CACHE_DIR "/report_moves.dat"
#define LIB_DOCUMENTS_FILE  LIB_CACHE_DIR "/documents.dat"
#define LIB_IMAGES_FILE     LIB_CACHE_DIR "/images.dat"
#define LIB_SPIKE_RUN_FILE  LIB_CACHE_DIR "/spike_run.dat"
#define LIB_COVERS_EMPTY_FILE LIB_ART_DIR "/covers_empty.pfraw"
#define LIB_NO_ART_ALBUMS_FILE  LIB_ART_DIR "/no_art_albums.dat"
#define LIB_NO_ART_ARTISTS_FILE LIB_ART_DIR "/no_art_artists.dat"

#define LIB_TAGCACHE_LOG    LIB_LOGS_DIR "/tagcache.log"
#define LIB_ART_LOG         LIB_LOGS_DIR "/art.log"
#define LIB_USB_LOG         LIB_LOGS_DIR "/usb.log"
#define LIB_BUFFER_LOG      LIB_LOGS_DIR "/buffer_damage.log"
#define LIB_UPGRADE_LOG     LIB_LOGS_DIR "/upgrade.log"

/* The libfile magic of each .dat, and the version of its records where the
 * file's own module does not set one. Records of size 1 are a byte stream
 * of entries that each say their own length. */
#define LIB_FORMAT_MAGIC    0x544f594cu     /* "LYOT"; version: the layout */
#define LIB_STAMPS_MAGIC    0x53545241u     /* "ARTS"; version: the format */
#define LIB_ALBUM_PLAYS_MAGIC 0x504c4241u   /* "ABLP" */
#define LIB_ALBUM_PLAYS_VERSION 1
#define LIB_BADGES_MAGIC    0x53474442u     /* "BDGS" */
#define LIB_BADGES_VERSION  1
#define LIB_MOVES_MAGIC     0x45564f4du     /* "MOVE" */
#define LIB_MOVES_VERSION   1
#define LIB_PLAYS_MAGIC     0x53594c50u     /* "PLYS" */
#define LIB_PLAYS_VERSION   1
#define LIB_BOOKS_MAGIC     0x4b4f4f42u     /* "BOOK" */
#define LIB_BOOKS_VERSION   1
#define LIB_QUIZ_MAGIC      0x5a495551u     /* "QUIZ" */
#define LIB_QUIZ_VERSION    1
#define LIB_SPIKE_MAGIC     0x534b5053u     /* "SPKS"; and the run, "SPKR" */
#define LIB_SPIKE_RUN_MAGIC 0x524b5053u
#define LIB_SPIKE_VERSION   1
#define LIB_PATHS_MAGIC     0x48544150u     /* "PATH" */
#define LIB_PATHS_VERSION   1

/* Whether this boot has files to move into the layout above */
bool library_files_need_upgrade(void);

/* Creates any folder above that is missing, and moves the files of an older
 * layout into it. At boot, before anything reads them; progress is told as
 * done of total steps. */
void library_files_init(void (*progress)(int done, int total));

#endif /* _LIBRARY_FILES_H */
