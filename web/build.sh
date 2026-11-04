#!/usr/bin/env bash
#
# Builds the install gate for a browser and for Node, with Emscripten in
# Docker (web/dist/crinstall.mjs and crinstall.wasm), and the page that runs
# it (web/dist/site: serve it with any static server). The page fetches the
# keyed pack from its pack/ folder: PACK=<keyed.py's output> puts a pack
# there, with pack.json (its firmware build and the app's version).
#
#   web/build.sh          the gate and the page
#   web/build.sh test     also the Node test (web/test/install.mjs): the
#                         repository's zip (CHORENSHA_ZIP) and a pack (PACK)
#                         made into /CHORENSHA's files, compared with the
#                         host's (CHORENSHA_REFERENCE, CHORENSHA_IMAGES)
#                         when set
#
# EMSDK_IMAGE overrides the pinned Emscripten image.

set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMSDK_IMAGE="${EMSDK_IMAGE:-emscripten/emsdk:6.0.11}"

run() {
  docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src "$@" "$EMSDK_IMAGE" \
    "${CMD[@]}"
}

CFLAGS="-std=c11 -O3 -Wall -Wextra -Werror -Wno-unknown-pragmas"
SOURCES="rp/src/install.c rp/src/install_check.c rp/src/zipread.c rp/src/sound_convert.c rp/src/crc32.c \
  rp/src/sha256.c rp/src/chacha20.c third_party/miniz/miniz_tinfl.c web/src/crweb.c"

mkdir -p "$ROOT/web/dist"
# shellcheck disable=SC2086
CMD=(emcc $CFLAGS -I web/src -I rp/src/include -I tools/xlat -I third_party/miniz
     -DMINIZ_NO_MALLOC -DMINIZ_NO_STDIO -DMINIZ_NO_TIME $SOURCES
     -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web,worker,node -sSTACK_SIZE=1048576
     -sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,HEAPU8
     -sEXPORTED_FUNCTIONS=_cr_install,_malloc,_free -o web/dist/crinstall.mjs)
run
echo "Built web/dist/crinstall.mjs and crinstall.wasm"

# The page: web/site's files, the gate and the pack, in web/dist/site.
rm -rf "$ROOT/web/dist/site"
mkdir -p "$ROOT/web/dist/site/pack"
cp "$ROOT"/web/site/* "$ROOT/web/dist/crinstall.mjs" "$ROOT/web/dist/crinstall.wasm" \
  "$ROOT/web/dist/site/"
if [ -n "${PACK:-}" ]; then
  cp "$PACK"/*.IMK "$PACK"/*.STK "$ROOT/web/dist/site/pack/"
  FIRMWARE="$(dd if="$PACK/COMMON.IMK" bs=1 skip=32 count=32 2>/dev/null | tr -d '\0')"
  printf '{"firmware": "%s", "version": "%s"}\n' "$FIRMWARE" \
    "$(cat "$ROOT/version.txt")" > "$ROOT/web/dist/site/pack/pack.json"
  echo "The pack for $FIRMWARE in web/dist/site/pack"
fi
echo "Built the page in web/dist/site"

if [ "${1:-}" = "test" ]; then
  CMD=(node web/test/install.mjs)
  OPTIONAL=()
  if [ -n "${CHORENSHA_REFERENCE:-}" ]; then
    OPTIONAL+=(-e CHORENSHA_REFERENCE=/reference -v "$CHORENSHA_REFERENCE:/reference")
  fi
  if [ -n "${CHORENSHA_IMAGES:-}" ]; then
    OPTIONAL+=(-e CHORENSHA_IMAGES=/images -v "$CHORENSHA_IMAGES:/images")
  fi
  run -e CHORENSHA_ZIP=/zip/repository.zip -e PACK=/pack -v "$PACK:/pack" \
      -v "$CHORENSHA_ZIP:/zip/repository.zip" ${OPTIONAL[@]+"${OPTIONAL[@]}"}
fi
