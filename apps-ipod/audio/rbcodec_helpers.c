/***************************************************************************
 * Original code from RockBox
 * was: apps/rbcodec_helpers.c
 * Copyright (C) 2006 by Nicolas Pitre <nico@cam.org>
 * Copyright (C) 2006-2007 by Stéphane Doyon <s.doyon@videotron.ca>
 * Copyright (C) 2012 Michael Sevakis
 * GNU General Public License (version 2+)
 *
 * Glue the shared rbcodec library expects from the application: buffer
 * allocation for the time-stretch DSP.
 ****************************************************************************/

#include "platform.h"
#include "dsp_core.h"
#include "core_alloc.h"
#include "tdspeed.h"

static int handles[4] = { 0, 0, 0, 0 };

/* core_alloc is a compacting allocator: it may relocate a block to close a
 * gap, so the pointer core_get_data() returned earlier goes stale. Blocks
 * allocated with core_alloc_ex() name a move_callback, which buflib calls
 * with the old and new addresses so the owner can re-point anything that
 * still refers to the block. */
static int move_callback(int handle, void *current, void *new)
{
    /* Never refuses. A move can only land here while the owner has yielded,
     * and the DSP loop finishes an iteration before it yields and restarts
     * from its input buffer -- so there is no partly-processed state pointing
     * into the block. If that ever stops holding, this needs to return
     * BUFLIB_CB_CANNOT_MOVE while the DSP is busy. */
    for (unsigned int i = 0; i < ARRAYLEN(handles); i++)
    {
        if (handle != handles[i])
            continue;

        tdspeed_move(i, current, new);
        break;
    }

    return BUFLIB_CB_OK;
}

static struct buflib_callbacks ops =
{
    .move_callback = move_callback,
    .shrink_callback = NULL,
};

/* Allocate timestretch buffers */
bool tdspeed_alloc_buffers(int32_t **buffers, const int *buf_s, int nbuf)
{
    /*  #Buffer index - 0 ovl L, 1 ovl R, 2 out L, 3 out R */
    for (int i = 0; i < nbuf; i++)
    {
        if (handles[i] <= 0)
        {
            handles[i] = core_alloc_ex(buf_s[i], &ops);

            if (handles[i] <= 0)
                return false;
        }

        if (buffers[i] == NULL)
        {
            buffers[i] = core_get_data(handles[i]);

            if (buffers[i] == NULL)
                return false;
        }
    }

    return true;
}

/* Free timestretch buffers */
void tdspeed_free_buffers(int32_t **buffers, int nbuf)
{
    for (int i = 0; i < nbuf; i++)
    {
        handles[i] = core_free(handles[i]);
        buffers[i] = NULL;
    }
}

