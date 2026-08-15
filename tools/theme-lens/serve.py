#!/usr/bin/env python3
"""PodBox Theme Lens, hosted locally against a theme folder on disk.

    python serve.py                 # find the theme folder, or ask for one
    python serve.py path/to/theme   # or name it
    python serve.py --tab           # a browser tab instead of an app window
    python serve.py --no-browser    # nothing; open the URL yourself

By default this opens in a window of its own and closing it stops the server, so
it behaves like an application rather than a site you have to remember to shut
down. With pywebview installed that is a native window; without it, a chromeless
browser window; failing both, an ordinary tab. On Windows, ThemeLens.cmd does
the same from a double-click, with no console.

The plain web build can only read a folder you hand it through a file picker,
and can never write one back, so every edit has to be re-uploaded. Hosted, the
same page reads and writes the files directly: Save writes the skin to disk, and
a change made in any other editor is picked up and re-rendered without a reload.

Standard library only. Nothing leaves the machine — the server binds to
localhost and refuses any path outside the folder it was pointed at.
"""

import argparse
import json
import mimetypes
import os
import queue
import shutil
import socket
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "index.html")

# What Save is allowed to write. Everything else in a theme is a font or a
# bitmap that this tool only ever reads, and a truncating write to one of those
# is not a mistake worth leaving available.
WRITABLE = {".wps", ".sbs", ".fms", ".rwps", ".rsbs", ".rfms", ".cfg", ".txt", ".md"}

# Directories that are never part of a theme.
SKIP_DIRS = {".git", ".svn", "__pycache__", "node_modules", ".idea"}

POLL_SECONDS = 0.5
MAX_FILES = 20000


def find_theme_root(start):
    """A theme folder is the one holding .rockbox. Accept either it or .rockbox
    itself, and look one level down before giving up."""
    start = os.path.abspath(start)
    if os.path.basename(start).lower() == ".rockbox":
        return os.path.dirname(start)
    if os.path.isdir(os.path.join(start, ".rockbox")):
        return start
    try:
        for name in sorted(os.listdir(start)):
            sub = os.path.join(start, name)
            if os.path.isdir(sub) and os.path.isdir(os.path.join(sub, ".rockbox")):
                return sub
    except OSError:
        pass
    return start


def ask_for_folder():
    """No folder to work on and nowhere obvious to look. Tkinter is in the
    standard library, so this costs nothing and makes the double-click case
    work; without it, launching from a shortcut would just fail silently."""
    try:
        import tkinter
        from tkinter import filedialog
    except ImportError:
        return None
    root = tkinter.Tk()
    root.withdraw()
    try:
        picked = filedialog.askdirectory(title="PodBox Theme Lens — choose a theme folder "
                                               "(the one containing .rockbox)")
    finally:
        root.destroy()
    return picked or None


# Where a chromeless window can come from, best first. Any Chromium understands
# --app; Firefox and Safari have no equivalent, so those fall back to a tab.
BROWSERS = {
    "win32": [
        r"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe",
        r"%ProgramFiles%\Microsoft\Edge\Application\msedge.exe",
        r"%ProgramFiles%\Google\Chrome\Application\chrome.exe",
        r"%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe",
        r"%LocalAppData%\Google\Chrome\Application\chrome.exe",
        r"%ProgramFiles%\BraveSoftware\Brave-Browser\Application\brave.exe",
    ],
    "darwin": [
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
        "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
    ],
    "linux": ["google-chrome", "chromium", "chromium-browser", "microsoft-edge", "brave-browser"],
}


def find_browser():
    for cand in BROWSERS.get(sys.platform, BROWSERS["linux"]):
        path = os.path.expandvars(cand)
        if os.path.isfile(path):
            return path
        found = shutil.which(cand)
        if found:
            return found
    return None


def run_webview_window(url, title):
    """A real native window, if pywebview is installed.

    Better than a chromeless browser in every way that shows: a proper title
    bar and taskbar entry, no browser profile on disk, and no second process to
    keep track of. It renders in WebView2 on Windows and WebKit elsewhere, so
    the page is the same page. Returns when the window is closed, or False at
    once if pywebview is not available.

    webview.start() has to own the main thread, which is why the HTTP server is
    already running in one of its own by the time this is called.
    """
    try:
        import webview
    except ImportError:
        return False
    try:
        webview.create_window(title, url, width=1400, height=900, min_size=(900, 600))
        webview.start()
    except Exception as err:                 # no WebView2 runtime, no GTK, ...
        print("  note   pywebview could not open a window (%s)" % err)
        return False
    return True


def open_app_window(url):
    """A window of its own, on a profile of its own.

    The separate profile is not tidiness — it guarantees a browser process this
    launch owns. Handed to an already-running copy, --app would open the window
    over there and this process would exit immediately, taking the server with
    it while the window was still on screen.
    """
    exe = find_browser()
    if not exe:
        return None
    profile = os.path.join(
        os.path.expandvars("%LocalAppData%") if sys.platform == "win32"
        else os.path.expanduser("~/.cache"),
        "PodBoxThemeLens", "browser")
    os.makedirs(profile, exist_ok=True)
    try:
        return subprocess.Popen([
            exe, "--app=" + url,
            "--user-data-dir=" + profile,
            "--no-first-run", "--no-default-browser-check",
            "--window-size=1400,900",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        return None


def free_port(host, wanted):
    """Take the port asked for, or the next one going. Two themes open at once
    is a normal thing to want, and a bare 'address in use' is not helpful."""
    for port in range(wanted, wanted + 20):
        with socket.socket() as s:
            try:
                s.bind((host, port))
                return port
            except OSError:
                continue
    return wanted


def walk(root):
    """Every file under root, as {relative posix path: (size, mtime_ns)}."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            try:
                st = os.stat(full)
            except OSError:
                continue
            out[rel] = (st.st_size, st.st_mtime_ns)
            if len(out) >= MAX_FILES:
                return out
    return out


class Watcher(threading.Thread):
    """Polls mtimes and fans changes out to every connected page.

    Polling rather than a native watcher because this has to work on a stock
    interpreter on three platforms, and half a second of latency on a file you
    just saved yourself is not something you can feel.
    """

    daemon = True

    def __init__(self, root):
        super().__init__(name="watcher")
        self.root = root
        self.lock = threading.Lock()
        self.clients = []           # list[queue.Queue]
        self.snapshot = walk(root)
        self.version = 1

    def subscribe(self):
        q = queue.Queue(maxsize=64)
        with self.lock:
            self.clients.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)

    def publish(self, payload):
        with self.lock:
            targets = list(self.clients)
        for q in targets:
            try:
                q.put_nowait(payload)
            except queue.Full:
                pass                # a page that far behind will resync on /api/tree

    def run(self):
        while True:
            time.sleep(POLL_SECONDS)
            try:
                now = walk(self.root)
            except OSError:
                continue
            changed = [p for p, v in now.items() if self.snapshot.get(p) != v]
            removed = [p for p in self.snapshot if p not in now]
            if not changed and not removed:
                continue
            self.snapshot = now
            self.version += 1
            self.publish({"version": self.version,
                          "changed": sorted(changed),
                          "removed": sorted(removed)})


class Control:
    """Drives the open page from outside it.

    A command is pushed down the same event stream the file watcher uses; the
    page runs it and posts the answer back, and the request that submitted it
    is unblocked. That is the whole mechanism — it means a script (or an agent)
    can open a skin, set the mock state, read the lint results and grab the
    rendered canvas without anyone touching the browser.
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.next_id = 1
        self.waiting = {}           # id -> (Event, box)

    def submit(self, bus, cmd, timeout):
        with self.lock:
            cid = self.next_id
            self.next_id += 1
            entry = (threading.Event(), {})
            self.waiting[cid] = entry
        bus.publish({"id": cid, "cmd": cmd})
        answered = entry[0].wait(timeout)
        with self.lock:
            self.waiting.pop(cid, None)
        if not answered:
            return {"ok": False, "error": "no page answered in %gs — is the tool open in a browser?" % timeout}
        return entry[1].get("result")

    def deliver(self, cid, result):
        with self.lock:
            entry = self.waiting.get(cid)
        if not entry:
            return False            # timed out, or a second page answering
        entry[1]["result"] = result
        entry[0].set()
        return True


class Handler(BaseHTTPRequestHandler):
    server_version = "ThemeLens"
    protocol_version = "HTTP/1.1"

    # ---- helpers -------------------------------------------------------

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("  %s\n" % (fmt % args))

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_bytes(self, body, ctype, code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def fail(self, code, msg):
        self.send_json({"error": msg}, code)

    def safe_path(self, rel):
        """Resolve a request path inside the theme root, or None.

        Every read and write goes through here — the server is on localhost but
        it is still a server, and a page in another tab can reach it."""
        rel = unquote(rel or "").lstrip("/")
        if not rel:
            return None
        root = self.server.root
        full = os.path.abspath(os.path.join(root, rel.replace("/", os.sep)))
        try:
            if os.path.commonpath([full, root]) != root:
                return None
        except ValueError:                       # different drives on Windows
            return None
        return full

    # ---- routes --------------------------------------------------------

    def do_GET(self):
        url = urlparse(self.path)
        path = url.path

        if path in ("/", "/index.html"):
            return self.serve_app()
        if path == "/api/info":
            return self.send_json({
                "root": self.server.root,
                "name": os.path.basename(self.server.root) or self.server.root,
                "version": self.server.watcher.version,
                "writable": sorted(WRITABLE),
            })
        if path == "/api/tree":
            return self.serve_tree()
        if path.startswith("/api/file/"):
            return self.serve_file(path[len("/api/file/"):])
        if path == "/api/events":
            return self.serve_events()
        return self.fail(404, "no such endpoint")

    def read_json(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return None
        try:
            return json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
        except (ValueError, UnicodeDecodeError):
            return None

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/api/control":
            body = self.read_json()
            if body is None:
                return self.fail(400, "expected a JSON command")
            timeout = float(body.pop("timeout", 10))
            result = self.server.control.submit(self.server.watcher, body, timeout)
            return self.send_json(result if isinstance(result, dict) else {"ok": True, "value": result})
        if path == "/api/result":
            body = self.read_json()
            if body is None or "id" not in body:
                return self.fail(400, "expected {id, result}")
            self.server.control.deliver(body["id"], body.get("result"))
            return self.send_json({"ok": True})
        return self.fail(404, "no such endpoint")

    def do_PUT(self):
        url = urlparse(self.path)
        if not url.path.startswith("/api/file/"):
            return self.fail(404, "no such endpoint")
        rel = url.path[len("/api/file/"):]
        full = self.safe_path(rel)
        if not full:
            return self.fail(403, "path is outside the theme folder")
        if os.path.splitext(full)[1].lower() not in WRITABLE:
            return self.fail(403, "this tool only writes skin and config files")

        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return self.fail(400, "bad Content-Length")
        body = self.rfile.read(length) if length else b""

        # Write beside the target and replace, so an interrupted write cannot
        # leave a half-file where a working skin was.
        tmp = full + ".lens-tmp"
        try:
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(tmp, "wb") as fh:
                fh.write(body)
            os.replace(tmp, full)
        except OSError as err:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            return self.fail(500, str(err))

        rel_posix = os.path.relpath(full, self.server.root).replace(os.sep, "/")
        print("  saved  %s  (%d bytes)" % (rel_posix, len(body)))
        return self.send_json({"ok": True, "path": rel_posix, "bytes": len(body)})

    # ---- route bodies --------------------------------------------------

    def serve_app(self):
        try:
            with open(APP, "rb") as fh:
                body = fh.read()
        except OSError:
            return self.fail(500, "index.html is missing from %s" % HERE)
        return self.send_bytes(body, "text/html; charset=utf-8")

    def serve_tree(self):
        snap = walk(self.server.root)
        files = [{"path": p, "size": v[0], "mtime": v[1] // 1000000}
                 for p, v in sorted(snap.items())]
        return self.send_json({
            "root": self.server.root,
            "version": self.server.watcher.version,
            "files": files,
        })

    def serve_file(self, rel):
        full = self.safe_path(rel)
        if not full or not os.path.isfile(full):
            return self.fail(404, "no such file")
        try:
            with open(full, "rb") as fh:
                body = fh.read()
        except OSError as err:
            return self.fail(500, str(err))
        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        return self.send_bytes(body, ctype)

    def serve_events(self):
        """Server-sent events: one line per on-disk change."""
        q = self.server.watcher.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        try:
            self.wfile.write(b": connected\n\n")
            self.wfile.flush()
            while True:
                try:
                    payload = q.get(timeout=15)
                    frame = "data: %s\n\n" % json.dumps(payload)
                except queue.Empty:
                    frame = ": keepalive\n\n"    # keeps proxies and idle sockets honest
                self.wfile.write(frame.encode("utf-8"))
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass                                 # the page went away
        finally:
            self.server.watcher.unsubscribe(q)
        self.close_connection = True


class LensServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        """Reloading the page drops its keep-alive sockets mid-flight, and the
        event stream goes with it every time. Here those are the normal course
        of events rather than faults — report anything else."""
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionResetError, ConnectionAbortedError,
                            BrokenPipeError, TimeoutError)):
            return
        super().handle_error(request, client_address)


def main():
    ap = argparse.ArgumentParser(description="Host PodBox Theme Lens against a theme folder.")
    ap.add_argument("root", nargs="?",
                    help="the theme folder — the one containing .rockbox (default: look from here, "
                         "and ask if there is nothing to find)")
    ap.add_argument("--port", type=int, default=8781)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--tab", action="store_true",
                    help="open a browser tab instead of an app window; the server outlives it")
    ap.add_argument("--no-browser", action="store_true", help="open nothing")
    ap.add_argument("--no-webview", action="store_true",
                    help="skip pywebview and use a chromeless browser window instead")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every request")
    args = ap.parse_args()

    if not os.path.isfile(APP):
        sys.exit("index.html is not next to serve.py (looked in %s)" % HERE)

    if args.root:
        root = find_theme_root(args.root)
    else:
        root = find_theme_root(".")
        # Nothing theme-shaped here, so this was almost certainly launched from
        # a shortcut rather than from inside a theme. Ask.
        if not os.path.isdir(os.path.join(root, ".rockbox")):
            picked = ask_for_folder()
            if not picked:
                sys.exit("no theme folder chosen")
            root = find_theme_root(picked)
    if not os.path.isdir(root):
        sys.exit("not a folder: %s" % root)

    args.port = free_port(args.host, args.port)

    watcher = Watcher(root)
    watcher.start()

    httpd = LensServer((args.host, args.port), Handler)
    httpd.root = root
    httpd.watcher = watcher
    httpd.control = Control()
    httpd.verbose = args.verbose

    url = "http://%s:%d/" % (args.host, args.port)
    n = len(watcher.snapshot)
    print("PodBox Theme Lens")
    print("  theme  %s" % root)
    print("  files  %d%s" % (n, " (listing capped)" if n >= MAX_FILES else ""))
    if not os.path.isdir(os.path.join(root, ".rockbox")):
        print("  note   no .rockbox here — pass the theme folder if this is the wrong one")
    print("  url    %s" % url)
    print("  Save writes the skin back to disk; edits made elsewhere reload themselves.")
    print("  Drive it from a script with:  python ctl.py --port %d status" % args.port)

    # The window owns the main thread from here, so the server moves off it.
    threading.Thread(target=httpd.serve_forever, name="http", daemon=True).start()

    def wait_for_interrupt():
        print("  Ctrl-C to stop.")
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass

    if args.no_browser:
        wait_for_interrupt()
    elif args.tab:
        webbrowser.open(url)
        wait_for_interrupt()
    else:
        print("  Close the window to stop the server.")
        title = "PodBox Theme Lens — %s" % (os.path.basename(root) or root)
        opened = False
        if not args.no_webview:
            opened = run_webview_window(url, title)      # blocks until closed
        if not opened:
            child = open_app_window(url)
            if child is not None:
                child.wait()
                opened = True
        if not opened:
            print("  note   nothing could open a window — opening a browser tab instead")
            webbrowser.open(url)
            wait_for_interrupt()

    httpd.shutdown()
    print("\nstopped")


if __name__ == "__main__":
    main()
