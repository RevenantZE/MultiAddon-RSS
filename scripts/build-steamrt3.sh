#!/usr/bin/env bash
set -euo pipefail
deps=$(realpath "${1:?dependency directory required}")
out=$(realpath -m "${2:?new output directory required}")
src=$(cd "$(dirname "$0")/.." && pwd -P)
test "$(git -C "$src" rev-parse --show-toplevel)" = "$src"
test -z "$(git -C "$src" status --porcelain)"
test ! -e "$out"
case "$out/" in "$src/"*|"$deps/"*) echo 'Output must be outside source/deps' >&2; exit 2;; esac
check_dep() {
  test "$(git -C "$deps/$1" rev-parse HEAD)" = "$2"
  test -z "$(git -C "$deps/$1" status --porcelain)"
}
check_dep mmsource-2.0 fa6f80e4662e5b96cc2e97722d812f374581dfd8
check_dep hl2sdk-cs2 bd17582be4bc18970e4fe4c518359872fda8276d
check_dep hl2sdk-manifests 25562f7019dbe9534a61a5e8b1959ea4e36a6e9e
check_dep mmsource-2.0/third_party/khook 40d233d160b5bf60cc3e732939142b222fbd8ece
mkdir -p "$out"
# The pinned hl2sdk lags CS2 vtable changes. Build against a copy of it with
# patches/sdk/*.patch applied; the dependency checkout stays clean.
sdk="$out/hl2sdk-cs2-patched"
mkdir "$sdk"
git -C "$deps/hl2sdk-cs2" archive HEAD | tar -x -C "$sdk"
shopt -s nullglob
for patch in "$src"/patches/sdk/*.patch; do
  GIT_CEILING_DIRECTORIES="$out" git -C "$sdk" apply --ignore-whitespace "$patch"
  echo "Applied $(basename "$patch") to $sdk"
done
shopt -u nullglob
docker run --rm --user "$(id -u):$(id -g)" \
  -e HOME=/tmp/mam-home -e PYTHONDONTWRITEBYTECODE=1 -e CC=clang -e CXX=clang++ \
  -e GIT_DIR=/src/MultiAddonManager/.git -e GIT_WORK_TREE=/src/MultiAddonManager \
  -v "$src:/src/MultiAddonManager:ro" -v "$deps:/deps:ro" -v "$out:/evidence:rw" \
  -v "$sdk:/deps/hl2sdk-cs2:ro" \
  -w /evidence ghcr.io/source2ze/build-containers@sha256:fe730bcab225db7a11ba8c185c868afc715445552bca33f5615acb4945a41751 bash -lc '
set -euo pipefail
step() { set +e; "$@"; rc=$?; set -e; printf "EXIT:%s COMMAND:%s\n" "$rc" "$1"; test "$rc" -eq 0; }
step clang++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I/src/MultiAddonManager/src /src/MultiAddonManager/tests/rss_preferences_tests.cpp -o /evidence/rss_preferences_tests
step /evidence/rss_preferences_tests
step clang++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I/src/MultiAddonManager/src /src/MultiAddonManager/tests/rss_asset_flow_tests.cpp -o /evidence/rss_asset_flow_tests
step /evidence/rss_asset_flow_tests
step python /src/MultiAddonManager/configure.py --enable-optimize --sdks cs2 --targets x86_64 --mms_path /deps/mmsource-2.0 --hl2sdk-root /deps --hl2sdk-manifests /deps/hl2sdk-manifests
step ambuild -j 1
step ambuild -j 1
'
