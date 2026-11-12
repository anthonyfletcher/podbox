# Optional artwork host tests

From a clean checkout, run:

```sh
python3 tools/tests/albumart/metadata.py
python3 tools/tests/albumart/decode.py
```

Dependencies: Python 3, GNU make, GCC with ASan/UBSan and SDL2 development
headers (`sdl2-config`). The runner configures the default ipod6g simulator's
headers, then compiles the production parsers with sanitizers. It does not
build or launch the simulator UI. All metadata fixtures are generated locally.
No device, network, private backup or audio library is required.
The decoder tests also need Pillow. They compile the image/stream pipeline
with ASan/UBSan and generate JPEG, PNG, GIF, BMP and WebP fixtures. They check
all eight EXIF orientations by dimensions and quadrant pixels, buffer limits,
and request cancellation while another decode runs without that callback.
Metadata helpers linked for byte-stream decoding are not instrumented in this
probe; the separate metadata probe instruments the affected parsers.

The default output is `build-art-tests/` in the checkout. Supply a directory
as the first argument to keep separate runs. Logs contain each command and
its output. Use a new output directory to preserve a failing baseline run.

The metadata probe executes the complete ID3 frame loop, FLAC block loop,
Vorbis comment reader and MP4 covr reader. Assertions cover front-cover
selection, UTF-16 description offsets, malformed front-cover fallback,
EXIF-first JPEG identification and truncated picture headers. These are
host parser tests; they do not establish playback or physical-device behavior.

Upstream has no `tools/tests/` convention. These tests are optional for the
maintainer and can be kept outside the submitted source patch.
