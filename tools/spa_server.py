#!/usr/bin/env python3
"""Local test server for the SPA tests (tools/spatest.py): static pages from
examples/spa/, a small JSON API, uploads, slow answers, a dropped connection
and a websocket echo -- stdlib only.

  tools/spa_server.py PORT          serve until killed
"""
import base64, hashlib, json, os, struct, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "examples", "spa")
COUNTER = {"n": 0}
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body=b"", ctype="application/json", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/api/hello":
            return self.send(200, {"hello": "world", "n": [1, 2, 3], "q": q})
        if u.path == "/api/count":
            COUNTER["n"] += 1
            return self.send(200, {"n": COUNTER["n"]})
        if u.path == "/api/slow":
            time.sleep(float(q.get("ms", "300")) / 1000)
            return self.send(200, {"slow": q.get("ms", "300"), "tag": q.get("tag", "")})
        if u.path == "/api/404":
            return self.send(404, {"error": "nothing here"})
        if u.path == "/api/500":
            return self.send(500, "boom", "text/plain")
        if u.path == "/api/nojson":
            return self.send(200, "<html>not json</html>", "text/html", {"X-Custom": "yes"})
        if u.path == "/api/drop":
            self.close_connection = True
            self.connection.close()
            return
        if u.path == "/api/echo-headers":
            return self.send(200, {"x-test": self.headers.get("X-Test"), "ua": self.headers.get("User-Agent")})
        if u.path == "/api/ws":
            return self.ws_echo()
        if u.path == "/page2":
            return self.send(200, "<!doctype html><title>p2</title><p>page two</p>", "text/html")
        # static
        rel = u.path.lstrip("/") or "index.html"
        path = os.path.normpath(os.path.join(ROOT, rel))
        if path.startswith(os.path.normpath(ROOT)) and os.path.isfile(path):
            ctype = "text/html" if path.endswith(".html") else "application/octet-stream"
            return self.send(200, open(path, "rb").read(), ctype)
        return self.send(404, {"error": "no such file"})

    def do_POST(self):
        u = urlparse(self.path)
        data = self.body()
        ct = self.headers.get("Content-Type", "")
        if u.path == "/api/post":
            try:
                parsed = json.loads(data) if "json" in ct else None
            except ValueError:
                parsed = None
            return self.send(201, {"ctype": ct.split(";")[0], "json": parsed, "raw": data.decode("utf8", "replace") if parsed is None else None})
        if u.path == "/api/upload":
            # crude multipart: report part names, filenames and sizes
            boundary = ct.split("boundary=")[-1].encode()
            parts = []
            for part in data.split(b"--" + boundary):
                if b"Content-Disposition" not in part:
                    continue
                head, _, content = part.partition(b"\r\n\r\n")
                content = content.rstrip(b"\r\n")
                name = head.split(b'name="')[1].split(b'"')[0].decode()
                fn = head.split(b'filename="')[1].split(b'"')[0].decode() if b"filename=" in head else None
                parts.append({"name": name, "filename": fn, "size": len(content), "head": content[:16].decode("latin1") if fn else content.decode("utf8", "replace")})
            return self.send(200, {"parts": parts})
        return self.send(404, {"error": "no such route"})

    def do_PUT(self):
        return self.send(200, {"method": "PUT", "len": len(self.body())})

    def do_PATCH(self):
        return self.send(200, {"method": "PATCH", "len": len(self.body())})

    def do_DELETE(self):
        return self.send(204)

    # --- websocket echo ---
    def ws_echo(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            return self.send(400, "ws only")
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        self.wfile.flush()
        self.close_connection = True
        origin = self.headers.get("Origin", "")
        self.ws_send(0x1, ("hello origin=" + origin).encode())
        while True:
            hdr = self.rfile.read(2)
            if len(hdr) < 2:
                return
            op = hdr[0] & 0x0F
            ln = hdr[1] & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self.rfile.read(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self.rfile.read(8))[0]
            mask = self.rfile.read(4) if hdr[1] & 0x80 else b"\0\0\0\0"
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(self.rfile.read(ln)))
            if op == 0x8:
                self.ws_send(0x8, struct.pack(">H", 4000 if payload[:0] == b"" else 1000))
                return
            if op == 0x1:
                text = payload.decode()
                if text == "bye":
                    self.ws_send(0x8, struct.pack(">H", 4001))
                    return
                self.ws_send(0x1, ("echo:" + text).encode())

    def ws_send(self, op, payload):
        n = len(payload)
        head = bytes([0x80 | op]) + (bytes([n]) if n < 126 else b"\x7e" + struct.pack(">H", n))
        self.wfile.write(head + payload)
        self.wfile.flush()


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H)
    srv.daemon_threads = True
    srv.serve_forever()
