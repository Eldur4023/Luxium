#!/usr/bin/env bash
# Vendors what Lux owns into Luxium (a real copy, not a symlink: see
# docs/ARCHITECTURE.md §4.1).
#
#   src/lux_script/*   the language: Lux's include/lux_script + src/lux_script,
#                      EXCEPT natives.hpp, natives.cpp, template.cpp -- the
#                      browser's own face of those, drifting on purpose
#   src/<dir>/...      the .luxp format: Lux's src/luxp/ (DOM, HTML and CSS
#                      parsers, codecs, verifier, translator), same paths
#
#   tools/sync_from_lux.sh           copy
#   tools/sync_from_lux.sh --check   only compare; exit 1 if anything differs
#
# LUX_DIR overrides where Lux is (default: ../Lux next to this repo).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
LUX=${LUX_DIR:-$HERE/../Lux}
CHECK=${1:-}
OWN="natives.hpp natives.cpp template.cpp"

pairs=()
for f in "$HERE"/src/lux_script/*; do
    b=$(basename "$f")
    case " $OWN " in *" $b "*) continue ;; esac
    if   [ -f "$LUX/include/lux_script/$b" ]; then pairs+=("$LUX/include/lux_script/$b|$f")
    elif [ -f "$LUX/src/lux_script/$b" ];     then pairs+=("$LUX/src/lux_script/$b|$f")
    else echo "warning: src/lux_script/$b does not exist in Lux" >&2
    fi
done
while IFS= read -r -d '' f; do
    rel=${f#"$LUX/src/luxp/"}
    pairs+=("$f|$HERE/src/$rel")
done < <(find "$LUX/src/luxp" -type f -print0)

differ=0
for p in "${pairs[@]}"; do
    src=${p%%|*}; dst=${p#*|}
    cmp -s "$src" "$dst" 2>/dev/null && continue
    differ=$((differ + 1))
    if [ "$CHECK" = "--check" ]; then
        echo "differs: ${dst#"$HERE/"}"
    else
        mkdir -p "$(dirname "$dst")"
        cp "$src" "$dst"
        echo "updated: ${dst#"$HERE/"}"
    fi
done
[ "$CHECK" = "--check" ] && [ "$differ" -gt 0 ] && exit 1
echo "${#pairs[@]} vendored files, $differ $([ "$CHECK" = "--check" ] && echo differ || echo updated)"
