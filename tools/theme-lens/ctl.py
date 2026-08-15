#!/usr/bin/env python3
"""Drive a running PodBox Theme Lens from the command line.

serve.py has to be running with the tool open in a browser — the commands are
executed by the page, so the answers are the page's own: the same viewport
table, the same lint results, the same pixels.

    python ctl.py status                      what is open, and how it looks
    python ctl.py skins                       every skin in the theme
    python ctl.py open Themify_2.sbs          open one (name, tail or full path)
    python ctl.py mode layout                 layout | pixels
    python ctl.py outlines off
    python ctl.py zoom 1                      0.5–4, or "fit"; also sets the shot size
    python ctl.py state --set screen=8 --set listArtRows=true
    python ctl.py presets                     the firmware's own lists
    python ctl.py presets --screen 7          just this screen's
    python ctl.py presets "File browser"      load one into the preview
    python ctl.py cfg                         which theme .cfg is in force
    python ctl.py cfg Themify_2.cfg           force a different one
    python ctl.py issues                      the linter's findings
    python ctl.py viewports                   geometry, including row placement
    python ctl.py select --label Row_Text
    python ctl.py shot preview.png            the preview canvas, as a PNG
    python ctl.py text > current.wps
    python ctl.py reload | save
    python ctl.py reloadPage                  re-read index.html itself
    python ctl.py eval "model.vps.length"

Exits non-zero when the page reports an error, so it composes in a script.
"""

import argparse
import base64
import json
import sys
import urllib.error
import urllib.request


def call(port, payload, timeout):
    payload = dict(payload)
    payload["timeout"] = timeout
    req = urllib.request.Request(
        "http://127.0.0.1:%d/api/control" % port,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout + 5) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.URLError as err:
        sys.exit("cannot reach the server on port %d — is serve.py running? (%s)" % (port, err))


def coerce(text):
    """--set takes shell words, but State holds numbers and booleans."""
    low = text.lower()
    if low in ("true", "on", "yes"):
        return True
    if low in ("false", "off", "no"):
        return False
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("op", help="status, skins, open, reload, save, text, mode, outlines, zoom, "
                              "state, presets, cfg, issues, viewports, select, shot, eval, ping")
    ap.add_argument("arg", nargs="?", help="the operation's argument (a path, a mode, an expression)")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                    help="a State field, for `state` (repeatable)")
    ap.add_argument("--label", help="viewport label, for `select`")
    ap.add_argument("--index", type=int, help="viewport index, for `select`")
    ap.add_argument("--screen", type=int, help="filter `presets` to one %cs screen")
    ap.add_argument("--port", type=int, default=8781)
    ap.add_argument("--timeout", type=float, default=15)
    ap.add_argument("--raw", action="store_true", help="print the value with no JSON quoting")
    args = ap.parse_args()

    cmd = {"op": args.op}
    if args.op in ("open",):
        if not args.arg:
            sys.exit("open needs a skin name")
        cmd["path"] = args.arg
    elif args.op == "cfg":
        if args.arg:
            cmd["path"] = args.arg
    elif args.op == "presets":
        if args.arg:
            cmd["name"] = args.arg
        elif args.screen is not None:
            cmd["screen"] = args.screen
    elif args.op == "mode":
        cmd["mode"] = args.arg or "layout"
    elif args.op == "outlines":
        cmd["on"] = coerce(args.arg or "on") is not False
    elif args.op == "zoom":
        if args.arg:
            cmd["level"] = args.arg
    elif args.op == "eval":
        if not args.arg:
            sys.exit("eval needs an expression")
        cmd["js"] = args.arg
    elif args.op == "state" and args.set:
        pairs = {}
        for item in args.set:
            if "=" not in item:
                sys.exit("--set wants KEY=VALUE, got %r" % item)
            k, v = item.split("=", 1)
            pairs[k] = coerce(v)
        cmd["set"] = pairs
    elif args.op == "select":
        if args.label:
            cmd["label"] = args.label
        if args.index is not None:
            cmd["index"] = args.index
        if "label" not in cmd and "index" not in cmd:
            sys.exit("select needs --label or --index")

    reply = call(args.port, cmd, args.timeout)
    if not reply.get("ok"):
        sys.exit("error: %s" % reply.get("error", "unknown"))
    value = reply.get("value")

    if args.op == "shot":
        out = args.arg or "preview.png"
        with open(out, "wb") as fh:
            fh.write(base64.b64decode(value["png"]))
        print("%s  %dx%d" % (out, value["width"], value["height"]))
        return

    if args.raw or isinstance(value, str):
        print(value if isinstance(value, str) else json.dumps(value))
    else:
        print(json.dumps(value, indent=2))


if __name__ == "__main__":
    main()
