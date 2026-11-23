/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The codec helpers the WAV codec calls, for codecs-wasm.make, in place of
 * lib/rbcodec/codecs/lib/codeclib.c. That file also defines memcpy(),
 * strcmp() and the rest for codecs, forwarding them through the codec api;
 * linked into the core those would replace the core's own, so a codec built
 * in here calls the core's libc directly instead.
 ****************************************************************************/
#include "codecs.h"
#include "codeclib.h"
#include "metadata.h"
#include "dsp_proc_entry.h"

int codec_init(void)
{
    return 0;
}

void codec_set_replaygain(const struct mp3entry *id3)
{
    struct dsp_replay_gains gains =
    {
        .track_gain = id3->track_gain,
        .album_gain = id3->album_gain,
        .track_peak = id3->track_peak,
        .album_peak = id3->album_peak,
    };

    ci->configure(REPLAYGAIN_SET_GAINS, (intptr_t)&gains);
}
