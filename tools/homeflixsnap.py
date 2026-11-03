#!/usr/bin/env python3
"""Captures static HTML snapshots of every Homeflix view into css-tests/homeflix/.

Serves Homeflix's public/ dir next to a canned /api, opens it in Luxium's CEF (Chromium) and, per route,
serialises the rendered DOM of <body> (scripts removed, images pointing at local PNGs).  The snapshots are
what tools/cssrun.py renders in both engines.

    DISPLAY=:99 tools/homeflixsnap.py [PUBLIC_DIR]
"""
import json, os, re, signal, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

sys.path.insert(0, os.path.dirname(__file__))
from cdp import Page

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PUB = sys.argv[1] if len(sys.argv) > 1 else "/home/burnt/Documentos/Github/Homeflix-Local/app/public"
OUT = os.path.join(ROOT, "css-tests", "homeflix")
PORT, CDP = 18799, 9345

ep = lambda i, n, t: {"id": f"ep{i}", "number": n, "title": t}
SERIES = [{"id": f"s{i}", "title": t, "categories": c} for i, (t, c) in enumerate([
    ("Breaking Bad", ["Drama"]), ("The Wire", ["Drama"]), ("Arcane", ["Animación", "Ciencia ficción"]),
    ("Dark", ["Ciencia ficción"]), ("Chernobyl", ["Drama"]), ("Cowboy Bebop", ["Animación"]),
    ("Severance con un título muy largo que no cabe", []), ("Fargo", []), ("Mr. Robot", ["Drama"]),
    ("Sherlock", []), ("Tokyo Vice", [])])]
SERIE = {"id": "s0", "title": "Breaking Bad", "categories": ["Drama", "Crimen"], "seasons": [
    {"number": 1, "name": "Temporada 1", "episodes": [ep(1, 1, "Piloto"), ep(2, 2, "El gato en la bolsa"),
        ep(3, 3, "Y la bolsa en el río con un título larguísimo para forzar el recorte del texto en la fila"), ep(4, 4, "Cáncer")]},
    {"number": 2, "name": "Temporada 2", "episodes": [ep(5, 1, "Siete treinta y siete")]},
    {"number": 3, "name": "Especiales", "episodes": []}]}
USERS = [{"id": 1, "name": "Jesús"}, {"id": 2, "name": "Ana"}, {"id": 3, "name": "Invitado"}]
STATS = {"episodes_watched": 42, "series_watched": 6, "series_completed": 2, "watch_seconds_total": 99000,
         "watch_seconds_month": 18000, "streak_days": 5, "favorite_serie": "Breaking Bad",
         "activity_by_day": {f"2026-10-{d:02d}": (d * 7) % 5 + 1 for d in range(1, 15)},
         "watch_seconds_by_day": {f"2026-10-{d:02d}": ((d * 7) % 5 + 1) * 3000 for d in range(1, 15)},
         "watch_time_by_serie": [{"serie_title": "Breaking Bad", "seconds": 40000}, {"serie_title": "Dark", "seconds": 20000}]}
SUMMARY = [{"serie_id": "s0", "serie_title": "Breaking Bad", "watching": {"id": "ep2"}, "watched_count": 1, "episodes_count": 5},
           {"serie_id": "s2", "serie_title": "Arcane", "watching": None, "watched_count": 9, "episodes_count": 9}]
CONT = [{"serie_id": "s0", "serie_title": "Breaking Bad", "episode": {"id": "ep2", "season_number": 1, "number": 2}}]
TRACKS = {"available": True, "audio": [{"index": 0, "title": "Español", "codec": "ac3"}, {"index": 1, "language": "eng", "codec": "aac"}],
          "subtitles": [{"index": 0, "title": "Español"}, {"index": 1, "language": "eng"}]}
API = {"/api/series": SERIES, "/api/series/s0": SERIE, "/api/categories": [{"name": "Drama", "count": 5}, {"name": "Animación", "count": 2}, {"name": "Ciencia ficción", "count": 2}],
       "/api/watch/sessions": [], "/api/users": USERS, "/api/users/1/continue": CONT, "/api/users/1/stats": STATS,
       "/api/users/1/summary": SUMMARY, "/api/users/1/state/s0": {"ep1": "watched"}, "/api/episodes/ep2/tracks": TRACKS,
       "/api/episodes/ep2/vinfo": {"codec": "h264", "pix_fmt": "yuv420p", "container": "matroska,webm", "audio": [{"codec": "ac3"}]},
       "/api/desktop/config": {"mode": "x"}}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self): self.do_GET()
    def do_GET(self):
        p = urlparse(self.path).path
        if p.startswith("/api/"):
            if re.search(r"/(caratula|fondo|avatar)$", p):
                return self.send_file(os.path.join(PUB, "placeholder_" + ("fondo" if p.endswith("fondo") else "caratula") + ".png"))
            body = json.dumps(API.get(p, [] if p.endswith("rescan") is False else None)).encode()
            self.send_response(200); self.send_header("Content-Type", "application/json"); self.end_headers(); self.wfile.write(body); return
        f = os.path.join(PUB, p.lstrip("/") or "index.html")
        if not os.path.isfile(f): f = os.path.join(PUB, "index.html")
        self.send_file(f)
    def send_file(self, f):
        data = open(f, "rb").read()
        ct = {"css": "text/css", "js": "application/javascript", "html": "text/html", "png": "image/png"}.get(f.rsplit(".", 1)[-1], "application/octet-stream")
        self.send_response(200); self.send_header("Content-Type", ct); self.end_headers(); self.wfile.write(data)


SERIALISE = r"""(() => {
  const b = document.body.cloneNode(true);
  b.querySelectorAll('script,#page-loader').forEach(e => e.remove());
  b.querySelectorAll('[style]').forEach(e => { if (/^(opacity|transition|transform)/.test(e.getAttribute('style')||'')) e.removeAttribute('style'); });
  let n = 0; b.querySelectorAll('[class],input,select,button,textarea,h1,h2,h3,video').forEach(e => { if (!e.closest('dialog,.app-toasts')) e.setAttribute('data-t', 'e' + (n++)); });
  let h = b.innerHTML;
  h = h.replace(/(src=|url\()(["']?)\/api\/[^"')\s?]*?(caratula|fondo|avatar)[^"')\s]*/g, (m, a, q, k) => a + q + (k === 'fondo' ? 'fondo.png' : 'poster.png'));
  return h.replace(/src="\/img\/logo.webp"/g, 'src="logo.webp"');
})()"""

ROUTES = {"picker": "/usuarios", "catalog": "/", "catalog-search": "/?q=a", "series": "/serie/s0", "player": "/serie/s0/ver/ep2",
          "profile": "/perfil", "new-series": "/series/nueva"}


def main():
    os.makedirs(OUT, exist_ok=True)
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    for n in ("caratula", "fondo"):
        subprocess.run(["cp", os.path.join(PUB, f"placeholder_{n}.png"), os.path.join(OUT, "poster.png" if n == "caratula" else "fondo.png")])
    subprocess.run(["cp", os.path.join(PUB, "img/logo.webp"), os.path.join(OUT, "logo.webp")])
    subprocess.run(["cp", os.path.join(PUB, "css/style.css"), os.path.join(OUT, "style.css")])
    env = dict(os.environ, LUXIUM_CDP_PORT=str(CDP))
    proc = subprocess.Popen([os.path.join(ROOT, "build", "luxium"), f"http://127.0.0.1:{PORT}/usuarios", "--size", "1180x820"], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        pg = Page(CDP)
        pg.eval("localStorage.setItem('homeflix:user', JSON.stringify({id:1,name:'Jesús'})), 1")
        for name, path in ROUTES.items():
            pg.eval(f"history.pushState({{}}, '', {json.dumps(path)}); renderRoute(true).then(()=>1)")
            time.sleep(1.0)
            html = pg.eval(SERIALISE)
            page = ('<!doctype html>\n<html lang="es"><head><meta charset="utf-8"><meta name="color-scheme" content="dark"><title>Homeflix %s</title>\n'
                    '<link rel="stylesheet" href="style.css"><style>*,*::before,*::after{transition:none!important;animation:none!important}</style></head>\n<body>\n%s\n</body></html>\n') % (name, html)
            open(os.path.join(OUT, f"t-hf-{name}.html"), "w").write(page)
            print("snapshot", name, len(html))
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        srv.shutdown()


if __name__ == "__main__":
    main()
