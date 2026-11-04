#!/usr/bin/env python3
"""Preview the Shelf Lite web app on a PC without a board.

    python3 tools/mock_server.py [folder]      then open http://localhost:8080

It serves web/index.html and copies the board's API, storing files in [folder]
(default: ./mock-shelf). Handy for working on the page.
"""
import json, os, sys, time, shutil, mimetypes, re, threading
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STORE = Path(sys.argv[1] if len(sys.argv) > 1 else "mock-shelf").resolve()
STORE.mkdir(parents=True, exist_ok=True)
(STORE / ".shelf").mkdir(exist_ok=True)
AUDIO = {"mp3", "m4a", "aac", "wav", "flac", "ogg", "opus"}
SPK = {"state": 0, "title": "", "path": "", "vol": 60}
SETTINGS = {"host": "shelf-ab12", "speaker": True}
T0 = time.time()

def real(p):
    p = "/" + p.strip().lstrip("/")
    if ".." in p: raise ValueError("bad path")
    return STORE / p.lstrip("/"), p.rstrip("/") or "/"

def hidden(n): return n.startswith(".") or n == "System Volume Information"

def music():
    out = []
    for dp, dns, fns in os.walk(STORE):
        dns[:] = sorted(d for d in dns if not hidden(d))
        for f in sorted(fns):
            e = f.rsplit(".", 1)[-1].lower()
            if hidden(f) or e not in AUDIO: continue
            full = Path(dp) / f
            rel = "/" + str(full.relative_to(STORE)).replace(os.sep, "/")
            folder = "" if Path(dp) == STORE else Path(dp).name
            out.append({"p": rel, "t": f.rsplit(".", 1)[0], "f": folder, "s": full.stat().st_size, "spk": int(e in ("mp3", "wav"))})
    return out

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass

    def j(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def err(self, code, msg): self.j({"ok": False, "error": msg}, code)

    def params(self):
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        return u.path, q

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def form(self, q):
        b = self.body()
        if self.headers.get("Content-Type", "").startswith("application/x-www-form-urlencoded"):
            q.update({k: v[0] for k, v in parse_qs(b.decode(), keep_blank_values=True).items()})
        return b

    def do_GET(self):
        path, q = self.params()
        if path == "/":
            b = (ROOT / "web" / "index.html").read_bytes()
            self.send_response(200); self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(b))); self.end_headers(); return self.wfile.write(b)
        if path == "/icon.png":
            b = (ROOT / "docs" / "icon.png").read_bytes()
            self.send_response(200); self.send_header("Content-Type", "image/png"); self.send_header("Content-Length", str(len(b))); self.end_headers(); return self.wfile.write(b)
        if path == "/manifest.webmanifest": return self.j({"name": "The Shelf Lite"})
        if path == "/api/info":
            tot = 31_900_000_000
            used = sum(f.stat().st_size for f in STORE.rglob("*") if f.is_file())
            return self.j({"ok": True, "board": "Heltec V4", "boardId": "v4", "fw": "1.0.0", "storage": "microSD card", "sd": True,
                           "total": tot, "used": used, "host": SETTINGS["host"], "ip": "192.168.1.57", "ssid": "HomeNet", "rssi": -58,
                           "ap": False, "apName": "Shelf-Lite-AB12", "uptime": int(time.time() - T0) + 7300, "heap": 201344,
                           "speaker": SETTINGS["speaker"], "songs": len(music()), "scanning": False})
        if path == "/api/list":
            try: d, p = real(q.get("path", "/"))
            except ValueError: return self.err(400, "bad path")
            if not d.is_dir(): return self.err(404, "no such folder")
            items = [{"n": c.name, "d": int(c.is_dir()), "s": 0 if c.is_dir() else c.stat().st_size, "t": int(c.stat().st_mtime)}
                     for c in d.iterdir() if not hidden(c.name)]
            return self.j({"ok": True, "path": p, "items": items})
        if path == "/api/music": return self.j(music())
        if path == "/api/board":
            f = STORE / ".shelf" / "board.json"
            return self.j(json.loads(f.read_text()) if f.exists() else [])
        if path == "/api/speaker": return self.j({"ok": True, "enabled": SETTINGS["speaker"], **SPK})
        if path == "/api/wifi/scan":
            return self.j({"ok": True, "scanning": False, "nets": [{"ssid": "HomeNet", "rssi": -52, "lock": 1}, {"ssid": "Garage", "rssi": -77, "lock": 1}, {"ssid": "Guest", "rssi": -70, "lock": 0}]})
        if path == "/f":
            try: f, p = real(q.get("p", ""))
            except ValueError: return self.err(400, "bad path")
            if not f.is_file(): return self.err(404, "not found")
            size = f.stat().st_size; start, end = 0, size - 1; code = 200
            rg = self.headers.get("Range")
            if rg and size:
                m = re.match(r"bytes=(\d*)-(\d*)", rg)
                a, b = m.group(1), m.group(2)
                if a == "": start = max(0, size - int(b))
                else: start = int(a); end = int(b) if b else end
                end = min(end, size - 1); code = 206
            self.send_response(code)
            self.send_header("Content-Type", mimetypes.guess_type(f.name)[0] or "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes"); self.send_header("Content-Length", str(end - start + 1))
            if code == 206: self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            with open(f, "rb") as fh:
                fh.seek(start); left = end - start + 1
                while left > 0:
                    chunk = fh.read(min(65536, left))
                    if not chunk: break
                    try: self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError): return
                    left -= len(chunk)
            return
        self.err(404, "not found")

    def do_POST(self):
        path, q = self.params()
        if path == "/api/upload":
            raw = self.body()
            ct = self.headers.get("Content-Type", "")
            bnd = ct.split("boundary=")[-1].encode()
            for part in raw.split(b"--" + bnd):
                if b"filename=" not in part: continue
                head, _, data = part.partition(b"\r\n\r\n")
                name = re.search(rb'filename="([^"]*)"', head).group(1).decode()
                data = data[:-2] if data.endswith(b"\r\n") else data
                d, _ = real(q.get("dir", "/"))
                (d / Path(name).name).write_bytes(data)
            return self.j({"ok": True})
        if path == "/api/board":
            b = self.body()
            (STORE / ".shelf" / "board.json").write_bytes(b); return self.j({"ok": True})
        self.form(q)
        if path == "/api/mkdir":
            d, _ = real(q["path"]);
            if d.exists(): return self.err(409, "already exists")
            d.mkdir(); return self.j({"ok": True})
        if path == "/api/delete":
            d, p = real(q["path"])
            if p == "/": return self.err(400, "bad path")
            shutil.rmtree(d) if d.is_dir() else d.unlink(); return self.j({"ok": True})
        if path == "/api/rename":
            a, _ = real(q["from"]); b, _ = real(q["to"])
            if b.exists(): return self.err(409, "a file with that name already exists")
            a.rename(b); return self.j({"ok": True})
        if path == "/api/speaker":
            if q.get("vol"): SPK["vol"] = int(q["vol"])
            c = q.get("cmd", "")
            if c == "play": SPK.update(state=1, path=q["path"], title=Path(q["path"]).stem)
            elif c == "pause": SPK["state"] = 2
            elif c == "resume": SPK["state"] = 1
            elif c == "stop": SPK.update(state=0, title="", path="")
            return self.j({"ok": True})
        if path == "/api/settings":
            if q.get("speaker"): SETTINGS["speaker"] = q["speaker"] == "1"
            restart = bool(q.get("host")) and q["host"] != SETTINGS["host"]
            if q.get("host"): SETTINGS["host"] = q["host"]
            return self.j({"ok": True, "restart": restart})
        if path in ("/api/rescan", "/api/reboot", "/api/wifi", "/api/wifi/forget"): return self.j({"ok": True})
        self.err(404, "not found")

if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8080))
    print(f"Shelf Lite preview on http://localhost:{port}  (files in {STORE})")
    ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()
