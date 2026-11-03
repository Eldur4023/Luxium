#!/usr/bin/env python3
"""Static file server with Range support (the media fixtures need it; http.server has none).

    tools/rangeserver.py PORT [DIR]
"""
import http.server, os, re, sys, functools


class H(http.server.SimpleHTTPRequestHandler):
    def send_head(self):
        p = self.translate_path(self.path)
        if not os.path.isfile(p):
            return super().send_head()
        size = os.path.getsize(p)
        rng = self.headers.get("Range")
        f = open(p, "rb")
        m = re.match(r"bytes=(\d*)-(\d*)", rng) if rng else None
        if m:
            a = int(m.group(1)) if m.group(1) else 0
            b = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {a}-{b}/{size}")
            f.seek(a)
            self.rem = b - a + 1
        else:
            self.send_response(200)
            self.rem = size
        self.send_header("Content-Length", str(self.rem))
        self.send_header("Content-Type", self.guess_type(p))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        return f

    def copyfile(self, src, dst):
        n = self.rem
        while n > 0:
            d = src.read(min(65536, n))
            if not d:
                break
            try:
                dst.write(d)
            except OSError:
                break
            n -= len(d)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    d = sys.argv[2] if len(sys.argv) > 2 else "."
    http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])),
                                    functools.partial(H, directory=d)).serve_forever()
