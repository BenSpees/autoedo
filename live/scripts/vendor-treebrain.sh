#!/bin/sh
# Vendor the AutoEDO engine -- the embeddable-library subset (make lib) --
# into a Treebrain checkout, byte-identical, the same convention as
# third_party/irconv. Treebrain compiles it as third_party/autoedo
# (target autoedo-embed) against ITS OWN vendored irconv and
# signalsmith-stretch copies, so the shared pieces stay single.
#
#   sh scripts/vendor-treebrain.sh /path/to/treeductor
#
# Re-run after any engine change that Treebrain should pick up; commit the
# result there. VENDORED-FROM records the engine commit and becomes the
# embedded build's AE_BUILD_ID, so status.engineBuild keeps answering
# "which engine am I actually running".

set -e
cd "$(dirname "$0")/.."

if [ -z "$1" ] || [ ! -d "$1/third_party" ]; then
    echo "usage: sh scripts/vendor-treebrain.sh /path/to/treeductor" >&2
    exit 2
fi
DST="$1/third_party/autoedo"

SRCS="app.c app.h attack_picks.h audio.h audio_params.h audio_embed.c \
      corrector.c corrector.h embed.c embed.h httpd.c httpd.h \
      ir_load.c ir_load.h json.c json.h polyf0.c polyf0.h \
      sampler.c sampler.h shifter.cpp shifter.h tuning.h yin.c yin.h"

mkdir -p "$DST/src" "$DST/generated"
for f in $SRCS; do
    cp "src/$f" "$DST/src/$f"
done
sh scripts/embed.sh web_index_html  web/index.html  > "$DST/generated/web_index.h"
sh scripts/embed.sh web_scales_json web/scales.json > "$DST/generated/web_scales.h"

GIT=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
printf '%s\n' "$GIT" > "$DST/VENDORED-FROM"

echo "vendored engine @$GIT -> $DST"
