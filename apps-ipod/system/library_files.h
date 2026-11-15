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

/* The layout these sources expect, which format.dat's version records. A
 * change to it adds upgrade steps and bumps this.
 *   1: library/, logs/, the libfile .dat files.
 *   2: the database in library/database; every file the player owns a .dat.
 *   3: no album index on disk: tagcache keeps the album tables.
 *   4: the text viewer's reading places in user/.
 *   5: the document and image lists carry each file's date. */
#define LIBRARY_FORMAT 5

#define LIB_FORMAT_FILE     LIB_DIR "/format.dat"
#define LIB_SOUND_FILE      LIB_DIR "/sound.dat"
#define LIB_SOUND_PART      LIB_DIR "/sound.part"
#define LIB_SOUND_CAL_FILE  LIB_DIR "/sound_calibration.dat"

#define LIB_PLAYS_FILE      LIB_USER_DIR "/plays.dat"
#define LIB_PLAYBACK_LOG    LIB_USER_DIR "/playback.log"
#define LIB_PLAYBACK_STEM   LIB_USER_DIR "/playback"
#define LIB_AUDIOBOOKS_FILE LIB_USER_DIR "/audiobooks.dat"
#define LIB_BOOKMARKS_FILE  LIB_USER_DIR "/bookmarks.dat"
#define LIB_BADGES_FILE     LIB_USER_DIR "/report_badges.dat"
#define LIB_READING_FILE    LIB_USER_DIR "/reading.dat"
#define LIB_READING_STATS_FILE LIB_USER_DIR "/reading_stats.dat"
#define LIB_LISTEN_STATS_FILE LIB_USER_DIR "/listen_stats.dat"
#define LIB_KNOWN_ARTISTS_FILE LIB_USER_DIR "/known_artists.txt"
#define LIB_PLAYER_NAME_FILE   LIB_USER_DIR "/player_name.txt"
#define LIB_TRIM_FILE       LIB_USER_DIR "/trim.txt"

/* Export and Import Modifications: Rockbox's own name and place, which its
 * Import reads, so a copy goes to other firmware and comes back */
#define LIB_EXPORT_FILE     ROCKBOX_DIR "/database_changelog.txt"

#define LIB_COVERS_FILE     LIB_CACHE_DIR "/covers.cfg"
#define LIB_REPORT_INDEX_FILE LIB_CACHE_DIR "/report_index.dat"
#define LIB_REPORT_MOVES_FILE LIB_CACHE_DIR "/report_moves.dat"
#define LIB_DOCUMENTS_FILE  LIB_CACHE_DIR "/documents.dat"
#define LIB_IMAGES_FILE     LIB_CACHE_DIR "/images.dat"
#define LIB_GAMES_FILE      LIB_CACHE_DIR "/games.dat"
#define LIB_BOOK_INDEX_FILE LIB_CACHE_DIR "/books.dat"
#define LIB_UNREADABLE_FILE LIB_CACHE_DIR "/unreadable.dat"
#define LIB_UNREADABLE_LIST LIB_CACHE_DIR "/unreadable.txt"
#define LIB_COVERS_EMPTY_FILE LIB_ART_DIR "/covers_empty.pfraw"
#define LIB_NO_ART_ALBUMS_FILE  LIB_ART_DIR "/no_art_albums.dat"
#define LIB_NO_ART_ARTISTS_FILE LIB_ART_DIR "/no_art_artists.dat"

#define LIB_TAGCACHE_LOG    LIB_LOGS_DIR "/tagcache.log"
#define LIB_ART_LOG         LIB_LOGS_DIR "/art.log"
#define LIB_USB_LOG         LIB_LOGS_DIR "/usb.log"
#define LIB_BATTERY_LOG     LIB_LOGS_DIR "/battery.csv"
#define LIB_BUFFER_LOG      LIB_LOGS_DIR "/buffer_damage.log"
#define LIB_UPGRADE_LOG     LIB_LOGS_DIR "/upgrade.log"
#define LIB_PLAYBACK_ERRORS_LOG LIB_LOGS_DIR "/playback_errors.log"
/* Written by lib/rbcodec/metadata/metadata.c, which spells the path out */
#define LIB_METADATA_LOG    LIB_LOGS_DIR "/metadata.log"
#define LIB_SKIN_LOG        LIB_LOGS_DIR "/theme.log"
#define LIB_TASKS_LOG       LIB_LOGS_DIR "/tasks.log"

/* The libfile magic of each .dat, and the version of its records where the
 * file's own module does not set one. Records of size 1 are a byte stream
 * of entries that each say their own length. */
#define LIB_FORMAT_MAGIC    0x544f594cu     /* "LYOT"; version: the layout */
#define LIB_STAMPS_MAGIC    0x53545241u     /* "ARTS"; version: the format */
#define LIB_BADGES_MAGIC    0x53474442u     /* "BDGS" */
#define LIB_BADGES_VERSION  1
#define LIB_MOVES_MAGIC     0x45564f4du     /* "MOVE" */
#define LIB_MOVES_VERSION   4       /* 1: no unplaced count; 2: no key;
                                       3: no renamed files */
#define LIB_PLAYS_MAGIC     0x53594c50u     /* "PLYS" */
#define LIB_PLAYS_VERSION   1
#define LIB_BOOKS_MAGIC     0x4b4f4f42u     /* "BOOK" */
#define LIB_BOOKS_VERSION   2       /* 1: books keyed by album alone */
#define LIB_BOOK_INDEX_MAGIC 0x58444e49u    /* "INDX" */
#define LIB_BOOK_INDEX_VERSION 3   /* 2: no series; 1: no subjects */
#define LIB_MARKS_MAGIC     0x4b52414du     /* "MARK" */
#define LIB_MARKS_VERSION   1
#define LIB_READING_MAGIC   0x44414552u     /* "READ" */
#define LIB_READING_VERSION 3       /* 2: no flags; 1: no parts */
#define LIB_READING_STATS_MAGIC 0x41545352u  /* "RSTA" */
#define LIB_READING_STATS_VERSION 1
#define LIB_LISTEN_STATS_MAGIC 0x4154534cu   /* "LSTA" */
#define LIB_LISTEN_STATS_VERSION 1
#define LIB_PATHS_MAGIC     0x48544150u     /* "PATH" */
#define LIB_PATHS_VERSION   1
#define LIB_UNREADABLE_MAGIC 0x44524e55u    /* "UNRD" */
#define LIB_UNREADABLE_VERSION 1

/* One line in logs/upgrade.log, for a carry-over made outside the steps */
void library_files_log(const char *fmt, ...);

/* Whether this boot has files to move into the layout above */
bool library_files_need_upgrade(void);

/* Creates any folder above that is missing, and moves the files of an older
 * layout into it. At boot, before anything reads them; progress is told as
 * done of total steps. */
void library_files_init(void (*progress)(int done, int total));

#endif /* _LIBRARY_FILES_H */
