"""Build small sanitized host probes with the target simulator's headers."""
from pathlib import Path
import os
import shlex
import subprocess
import sys
ROOT = Path(os.environ.get("ART_TEST_ROOT", Path(__file__).resolve().parents[3])).resolve()
OUT = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build-art-tests").resolve()
OUT.mkdir(parents=True, exist_ok=True)
def run(args, name, check=True):
    result = subprocess.run(list(map(str, args)), cwd=OUT, text=True, capture_output=True)
    (OUT / (name + ".log")).write_text(shlex.join(list(map(str, args))) + "\n" + result.stdout + result.stderr)
    if check and result.returncode:
        raise RuntimeError(name + "\n" + result.stdout + result.stderr)
    return result
BUILD = OUT / "config"
BUILD.mkdir(exist_ok=True)
if not (BUILD / "autoconf.h").exists():
    subprocess.run([str(ROOT / "tools/configure"), "--target=ipod6g", "--type=s", "--appsdir=apps-ipod"], cwd=BUILD, check=True, stdout=subprocess.DEVNULL)
dry = subprocess.run(["make", "-n", str(BUILD / "apps-ipod/draw/jpeg_load.o")], cwd=BUILD, text=True, capture_output=True, check=True)
line = next(x for x in dry.stdout.splitlines() if " -c " in x and "jpeg_load.c" in x)
flags = shlex.split(line)
if flags[0].endswith("ccache"):
    flags = flags[1:]
CC, FLAGS = flags[0], [x for x in flags[1:flags.index("-c")] if x != "-Os"]
FLAGS += ["-I" + str(ROOT), "-g", "-O1", "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections"]
def compile_sources(sources):
    objects = []
    for source in sources:
        source = Path(source)
        obj = OUT / (source.stem + ".o")
        extra = ["-D__PCTOOL__"] if source.name == "unicode.c" else []
        run([CC, *FLAGS, *extra, "-c", ROOT / source, "-o", obj], "compile-" + source.stem)
        objects.append(obj)
    return objects
def link(name, source, objects):
    path = OUT / (name + ".c")
    path.write_text(source)
    run([CC, *FLAGS, path, *objects, "-Wl,--gc-sections", "-o", OUT / name], "link-" + name)
    return OUT / name
