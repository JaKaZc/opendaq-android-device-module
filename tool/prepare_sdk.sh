#!/bin/sh
# Put a clone of the openDAQ SDK at a release with the patches in patches/
# applied, for the Android build:
#
#   tool/prepare_sdk.sh <clone> <source repository> <release>
#
# The clone is reset only when it is not in that state already. A reset gives
# the patched files a new time, and every SDK build that compiles from the
# clone would then compile them and every file that includes them again.

set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
CLONE="$1"
SOURCE="$2"
REF="$3"

if [ ! -d "$CLONE" ]; then
  git clone -q --no-hardlinks "$SOURCE" "$CLONE"
fi
if ! git -C "$CLONE" rev-parse -q --verify "$REF^{commit}" > /dev/null; then
  git -C "$CLONE" fetch -q --tags "$SOURCE"
fi
WANT="$(git -C "$CLONE" rev-parse "$REF^{commit}")"

# Ready: at the release, every patch applied, and no other change.
ready=yes
[ "$(git -C "$CLONE" rev-parse HEAD)" = "$WANT" ] || ready=no
expected=""
for patch in "$HERE"/patches/*.patch; do
  if [ "$ready" = yes ] && ! git -C "$CLONE" apply --reverse --check "$patch" 2> /dev/null; then
    ready=no
  fi
  expected="$expected$(git -C "$CLONE" apply --numstat "$patch" 2> /dev/null | cut -f3)
"
done
if [ "$ready" = yes ]; then
  changed="$(git -C "$CLONE" diff --name-only HEAD | sort)"
  wanted="$(printf '%s' "$expected" | grep -v '^$' | sort -u)"
  [ "$changed" = "$wanted" ] || ready=no
fi

if [ "$ready" = yes ]; then
  echo "$CLONE is at $REF with its patches"
  exit 0
fi
git -C "$CLONE" checkout -q --force "$REF"
for patch in "$HERE"/patches/*.patch; do
  git -C "$CLONE" apply "$patch"
  echo "applied $(basename "$patch") to $REF"
done
