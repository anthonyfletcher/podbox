# Optional artwork host tests

From a clean checkout, run:

```sh
python3 tools/tests/albumart/metadata.py
python3 tools/tests/albumart/decode.py
python3 tools/tests/albumart/itunes.py
python3 tools/tests/albumart/cache.py
python3 tools/tests/albumart/publication.py
python3 tools/tests/albumart/offers.py
python3 tools/tests/albumart/consumers.py
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
The iTunes probe generates 2,000 tracks and images, builds the production
indexes, and makes 10,000 lookups with bounded read counts. It checks valid
small thumbnails followed by missing, short or unsafe larger candidates,
decoded pixels, exact-path collision checks, allocation failure, short headers
and an isolated UTF-16 low surrogate. Its core allocator is a deterministic
host port. The cache probe extracts the production source-selection function
unchanged and supplies deterministic source/decode ports. It checks fallback,
files-only exclusion and the central-cover override. It also checks lazy
one-attempt-per-job indexing, failed-index preservation in passes/checks,
deferred check invalidation and a later job's retry. The real index probe
checks that valid empty artwork does not open or scan iTunesDB.
The publication probe uses real temporary files and injects open, write,
close and rename failures into the unchanged production writer. The offer
probe uses deterministic queue, storage and allocation ports for sleep/wake,
bounded retries, superseded offers and policy changes. The consumer probe
checks null metadata and playback-report generation invalidation. Browser/report
entries and missing-hash/hue caches must survive busy publications and clear
once at idle. The offer probe also runs Cover Flow's actual refresh block
between each simulated size publication, then checks one reload after idle.
These ports
do not run the firmware scheduler or UI.

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
