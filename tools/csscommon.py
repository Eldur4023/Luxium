# csscommon.py -- shared bits of cssrun.py / cssgolden.py.
#
# A test is an HTML page under css-tests/.  Per-test options live in one
# comment:   <!-- cssrun: threshold=3.5 solid=80 sizes=1180x820,800x700 click=a,b -->
#   threshold  allowed % of differing pixels        (default 2.0)
#   solid      allowed pixels inside solid regions   (default 50)
#   sizes      viewport sizes to test at             (default 1180x820)
#   click      ids clicked (LuxScript) before the shot; requires name.expected.html,
#              the static page Chromium renders as the golden for the end state
import re
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TESTS = REPO / "css-tests"
GOLDEN = TESTS / "golden"
TMP = Path(tempfile.gettempdir()) / "luxium-css"
TMP.mkdir(exist_ok=True)

DEFAULT_SIZE = "1180x820"
URL_BAR_H = 38       # kUrlH
STATUS_H = 20        # status strip below the page


def options(html: Path) -> dict:
    try:
        text = html.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return {}
    m = re.search(r"<!--\s*cssrun:(.*?)-->", text, re.S)
    out = {}
    if m:
        for k, v in re.findall(r"(\w+)\s*=\s*([^\s]+)", m.group(1)):
            out[k] = v
    return out


def sizes_of(html: Path):
    opt = options(html)
    return opt.get("sizes", DEFAULT_SIZE).split(",")


def tag(rel_noext: str, size: str) -> str:
    """Test id as printed/stored: plain for the default size, `id@WxH` otherwise."""
    return rel_noext if size == DEFAULT_SIZE else f"{rel_noext}@{size}"


def golden_png(rel_noext: str, size: str) -> Path:
    return GOLDEN / (tag(rel_noext, size) + ".png")


def golden_json(rel_noext: str, size: str) -> Path:
    return GOLDEN / (tag(rel_noext, size) + ".json")


def oracle_source(html: Path) -> Path:
    """The page Chromium renders for the golden: name.expected.html if present."""
    exp = html.with_name(html.stem + ".expected.html")
    return exp if exp.exists() else html


def find_tests():
    return sorted(p for p in TESTS.rglob("*.html")
                  if p.is_file() and not {"golden", "out", "ref"} & set(p.relative_to(TESTS).parts)
                  and not p.name.endswith(".expected.html"))
