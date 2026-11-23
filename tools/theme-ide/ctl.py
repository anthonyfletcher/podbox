#!/usr/bin/env python3
"""Drive a running PodBox Theme IDE from the command line.

serve.py has to be running with the IDE open in a browser -- the commands are
carried out by the page, so the answers are the page's own: the same problems,
the same viewports, the player's own pixels.

    python ctl.py status                      what is open, and what the player shows
    python ctl.py skins                       every skin in the theme
    python ctl.py open scrim.wps              open one (name, tail or full path)
    python ctl.py goto wps                    show a screen: menu, music, albums,
                                              wps, quick
    python ctl.py press menu --ms 1200        a player button: menu, select, play,
                                              back, forward, up, down
    python ctl.py player                      whether the player is running
    python ctl.py select --index 3            pick a viewport, as the Outline does
    python ctl.py outline                     the open skin's viewports, by name
    python ctl.py viewports                   the firmware's own viewport table
    python ctl.py issues                      the problems in the theme's skins
    python ctl.py shot player.png             the player's screen, as a PNG
    python ctl.py output                      what the firmware has printed
    python ctl.py text > current.wps
    python ctl.py save
    python ctl.py reloadPage                  load the page again
    python ctl.py eval "ide.selected"

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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("op", help="status, skins, open, goto, press, player, select, outline, "
                              "viewports, issues, shot, output, text, save, reloadPage, eval, ping")
    ap.add_argument("arg", nargs="?", help="the operation's argument (a path, a screen, an expression)")
    ap.add_argument("--index", type=int, help="viewport index, for `select`")
    ap.add_argument("--ms", type=int, help="how long `press` holds the button")
    ap.add_argument("--port", type=int, default=8790)
    ap.add_argument("--timeout", type=float, default=15)
    ap.add_argument("--raw", action="store_true", help="print the value with no JSON quoting")
    args = ap.parse_args()

    cmd = {"op": args.op}
    if args.op in ("open",):
        if not args.arg:
            sys.exit("open needs a skin name")
        cmd["path"] = args.arg
    elif args.op == "goto":
        if not args.arg:
            sys.exit("goto needs a screen: menu, music, albums, wps or quick")
        cmd["screen"] = args.arg
    elif args.op == "press":
        if not args.arg:
            sys.exit("press needs a button")
        cmd["button"] = args.arg
        if args.ms:
            cmd["ms"] = args.ms
    elif args.op == "eval":
        if not args.arg:
            sys.exit("eval needs an expression")
        cmd["js"] = args.arg
    elif args.op == "select":
        if args.index is None:
            sys.exit("select needs --index")
        cmd["index"] = args.index

    reply = call(args.port, cmd, args.timeout)
    if not reply.get("ok"):
        sys.exit("error: %s" % reply.get("error", "unknown"))
    value = reply.get("value")

    if args.op == "shot":
        out = args.arg or "player.png"
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
