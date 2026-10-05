"""Reads and writes .luxp containers from Python (format: page/luxp.hpp).

    "LUXP" 0x05, uv npages, { str name, blob document, blob sheet, blob program }*,
    uv nresources, { str name, str bytes }*
    blob = u8 zlib?, uv plain size, str data          uv = LEB128, str = uv len + bytes
"""
import zlib

VERSION = 5


def _uv(data, at):
    v, shift = 0, 0
    while True:
        b = data[at]; at += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, at
        shift += 7


def _str(data, at):
    n, at = _uv(data, at)
    return data[at:at + n], at + n


def read(path):
    """-> {"pages": [{"name", "document", "sheet", "program"}], "resources": [(name, bytes)]}, sections plain."""
    data = open(path, "rb").read()
    if data[:4] != b"LUXP" or data[4] != VERSION:
        raise ValueError("not a v%d .luxp" % VERSION)
    at = 5
    n, at = _uv(data, at)
    pages = []
    for _ in range(n):
        name, at = _str(data, at)
        page = {"name": name.decode()}
        for section in ("document", "sheet", "program"):
            z = data[at]; at += 1
            _, at = _uv(data, at)
            blob, at = _str(data, at)
            page[section] = zlib.decompress(blob) if z else blob
        pages.append(page)
    n, at = _uv(data, at)
    resources = []
    for _ in range(n):
        name, at = _str(data, at)
        body, at = _str(data, at)
        resources.append((name.decode(), body))
    return {"pages": pages, "resources": resources}


def _w_uv(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80); v >>= 7
    out.append(v)
    return bytes(out)


def _w_str(b):
    return _w_uv(len(b)) + b


def write(path, container, compress=True):
    out = bytearray(b"LUXP" + bytes([VERSION]))
    out += _w_uv(len(container["pages"]))
    for p in container["pages"]:
        out += _w_str(p["name"].encode())
        for section in ("document", "sheet", "program"):
            plain = p[section]
            z = compress and len(plain) >= 64
            out += bytes([1 if z else 0]) + _w_uv(len(plain)) + _w_str(zlib.compress(plain, 9) if z else plain)
    out += _w_uv(len(container["resources"]))
    for name, body in container["resources"]:
        out += _w_str(name.encode()) + _w_str(body)
    open(path, "wb").write(bytes(out))


def plain_bytes(container):
    """Every section of every page and every resource, concatenated."""
    out = b""
    for p in container["pages"]:
        out += p["document"] + p["sheet"] + p["program"]
    for _, body in container["resources"]:
        out += body
    return out
