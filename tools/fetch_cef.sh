#!/usr/bin/env bash
# Downloads the latest stable CEF "minimal" linux64 distribution into
# third_party/cef (gitignored).  Pass a version prefix to pin one: fetch_cef.sh 154
set -euo pipefail
cd "$(dirname "$0")/.."
want="${1:-}"
mkdir -p third_party
name=$(curl -fsS https://cef-builds.spotifycdn.com/index.json | python3 -c '
import json, sys
want = sys.argv[1]
for v in json.load(sys.stdin)["linux64"]["versions"]:
    if v["channel"] != "stable" or not v["cef_version"].startswith(want): continue
    for f in v["files"]:
        if f["type"] == "minimal": print(f["name"]); sys.exit()
sys.exit("no matching CEF build")' "$want")
echo "fetching $name"
curl -fL --progress-bar -o "third_party/$name" "https://cef-builds.spotifycdn.com/$(python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1]))' "$name")"
rm -rf third_party/cef third_party/_cef_x
mkdir third_party/_cef_x
tar -xjf "third_party/$name" -C third_party/_cef_x --strip-components=1
mv third_party/_cef_x third_party/cef
rm "third_party/$name"
echo "CEF in third_party/cef"
