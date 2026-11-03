#!/usr/bin/env python3
"""Tiny Chrome DevTools Protocol client for driving Luxium's CEF pages in tests.

    LUXIUM_CDP_PORT=9333 luxium http://...            # in one shell
    tools/cdp.py 9333 'document.title'                # in another

As a module: `Page(port)` then `.eval(expr)` (awaits promises, returns the value).
"""
import json, sys, time, urllib.request
import websocket


class Page:
    def __init__(self, port, match=None, wait=20):
        end = time.time() + wait
        while True:
            try:
                tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
                tabs = [t for t in tabs if t["type"] == "page" and (match is None or match in t["url"])]
                if tabs:
                    break
            except Exception:
                pass
            if time.time() > end:
                raise RuntimeError("no CEF page on the debugging port")
            time.sleep(0.3)
        self.ws = websocket.create_connection(tabs[0]["webSocketDebuggerUrl"], timeout=60, suppress_origin=True)
        self.n = 0

    def call(self, method, **params):
        self.n += 1
        self.ws.send(json.dumps({"id": self.n, "method": method, "params": params}))
        while True:
            m = json.loads(self.ws.recv())
            if m.get("id") == self.n:
                if "error" in m:
                    raise RuntimeError(m["error"])
                return m["result"]

    def eval(self, expr, await_promise=True):
        r = self.call("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=await_promise)
        if "exceptionDetails" in r:
            d = r["exceptionDetails"]
            raise RuntimeError(d.get("exception", {}).get("description") or d.get("text"))
        return r["result"].get("value")

    def wait_for(self, expr, timeout=20, every=0.2):
        end = time.time() + timeout
        while time.time() < end:
            v = self.eval(expr)
            if v:
                return v
            time.sleep(every)
        raise TimeoutError(expr)


if __name__ == "__main__":
    p = Page(int(sys.argv[1]))
    print(json.dumps(p.eval(sys.argv[2]), indent=1))
