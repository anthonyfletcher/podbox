"""Stand in for Piper 1.2's `piper --model M --json-input` with the newer
Piper (piper1-gpl, the `piper-tts` Python package), whose command line has
no JSON mode. build-voices.sh puts this first on PATH as `piper`.

tools/voice.pl writes one request a line, {"text": ..., "output_file": ...},
and for each reads a line from stdout, then one from stderr, so both are
answered and flushed per request.

usage: <piper-tts python> piper_json.py --model <voice.onnx> --json-input
"""
import json
import sys
import wave

from piper import PiperVoice


def main():
    args = sys.argv[1:]
    voice = PiperVoice.load(args[args.index("--model") + 1])
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        request = json.loads(line)
        with wave.open(request["output_file"], "wb") as out:
            voice.synthesize_wav(request["text"], out)
        print(request["output_file"], flush=True)
        print("piper_json: wrote " + request["output_file"],
              file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
