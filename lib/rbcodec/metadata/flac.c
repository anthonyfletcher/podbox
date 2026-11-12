/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Dave Chapman
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include "platform.h"

#include "metadata.h"
#include "metadata_common.h"
#include "metadata_parsers.h"
#include "logf.h"

#ifdef HAVE_ALBUMART

bool parse_flac_album_art(unsigned char *buf, int bytes_read, enum mp3_aa_type *type, int *picframe_pos)
{
    *type = AA_TYPE_UNKNOWN;
    *picframe_pos = 0;
    if (bytes_read < 8)
        return false;
    uint32_t mime_length = get_long_be(buf + 4);
    if (mime_length > (uint32_t)bytes_read - 8)
        return false;
    if ((mime_length == 10 && !memcmp(buf + 8, "image/jpeg", 10)) ||
        (mime_length == 9 && !memcmp(buf + 8, "image/jpg", 9)))
        *type = AA_TYPE_JPG;
    else if (mime_length == 9 && !memcmp(buf + 8, "image/png", 9))
        *type = AA_TYPE_PNG;
    uint32_t at = 8 + mime_length;
    if (at + 4 > (uint32_t)bytes_read)
        return false;
    uint32_t description = get_long_be(buf + at);
    if (description > (uint32_t)bytes_read - at - 4 ||
        at + 4 + description + 20 > (uint32_t)bytes_read)
        return false;
    *picframe_pos = at + 4 + description + 16;
    return *type != AA_TYPE_UNKNOWN;
}

#endif /* HAVE_ALBUMART */

bool get_flac_metadata(int fd, struct mp3entry* id3)
{
    /* A simple parser to read vital metadata from a FLAC file - length,
     * frequency, bitrate etc. This code should either be moved to a
     * seperate file, or discarded in favour of the libFLAC code.
     * The FLAC stream specification can be found at
     * http://flac.sourceforge.net/format.html#stream
     */

    /* Use the trackname part of the id3 structure as a temporary buffer */
    unsigned char* buf = (unsigned char *)id3->path;
    bool last_metadata = false;
    bool rc = false;

    if (!skip_id3v2(fd, id3) || (read(fd, buf, 4) < 4))
    {
        return rc;
    }

    if (memcmp(buf, "fLaC", 4) != 0)
    {
        return rc;
    }

    while (!last_metadata)
    {
        unsigned long i;
        int type;

        if (read(fd, buf, 4) != 4)
        {
            return rc;
        }

        last_metadata = buf[0] & 0x80;
        type = buf[0] & 0x7f;
        /* The length of the block */
        i = (buf[1] << 16) | (buf[2] << 8) | buf[3];

        if (type == 0)       /* 0 is the STREAMINFO block */
        {
            unsigned long totalsamples;

            if (i >= sizeof(id3->path) || read(fd, buf, i) != (int)i)
            {
                return rc;
            }

            id3->vbr = true;   /* All FLAC files are VBR */
            id3->filesize = ffilesize(fd);
            id3->frequency = (buf[10] << 12) | (buf[11] << 4)
                | ((buf[12] & 0xf0) >> 4);
            rc = true;  /* Got vital metadata */

            /* totalsamples is a 36-bit field, but we assume <= 32 bits are used */
            totalsamples = get_long_be(&buf[14]);

            if(totalsamples > 0)
            {
                /* Calculate track length (in ms) and estimate the bitrate (in kbit/s) */
                id3->length = ((int64_t) totalsamples * 1000) / id3->frequency;
                id3->bitrate = (id3->filesize * 8) / id3->length;
            }
            else if (totalsamples == 0)
            {
                id3->length = 0;
                id3->bitrate = 0;
            }
            else
            {
                logf("flac length invalid!");
                return false;
            }

        }
        else if (type == 4)  /* 4 is the VORBIS_COMMENT block */
        {
            /* The next i bytes of the file contain the VORBIS COMMENTS. */
            if (read_vorbis_tags(fd, id3, i) == 0)
            {
                return rc;
            }
        }
#ifdef HAVE_ALBUMART
        else if (type == 6) /* 6 is the PICTURE block */
        {
            unsigned int buf_size = MIN(sizeof(id3->path), i);
            off_t picture_pos = lseek(fd, 0, SEEK_CUR);
            int bytes_read = read(fd, buf, buf_size);
            if (bytes_read < 0)
                return false;
            i -= bytes_read;
            int picframe_pos;
            enum mp3_aa_type picture_format;
            if (parse_flac_album_art(buf, bytes_read, &picture_format,
                                     &picframe_pos))
            {
                unsigned picture_type = get_long_be(buf);
                unsigned length = get_long_be(buf + picframe_pos);
                if (length <= (unsigned)(i + bytes_read - picframe_pos - 4) &&
                    (!id3->has_embedded_albumart ||
                     (picture_type == 3 && id3->albumart_picture_type != 3)))
                {
                    id3->has_embedded_albumart = true;
                    id3->albumart.type = picture_format;
                    id3->albumart.size = length;
                    id3->albumart.pos = picture_pos + picframe_pos + 4;
                    id3->albumart_picture_type = picture_type;
                }
            }

            if (lseek(fd, i, SEEK_CUR) < 0)
            {
                return rc;
            }
        }
#endif
        else if (!last_metadata)
        {
            /* Skip to next metadata block */
            if (lseek(fd, i, SEEK_CUR) < 0)
            {
                return rc;
            }
        }
    }

    return true;
}
