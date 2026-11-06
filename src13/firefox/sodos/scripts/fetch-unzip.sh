#!/bin/sh
# fetch-unzip.sh - unzip local (sem root) em vendor/tools; o configure do Firefox
# exige. O build-browser64.sh ja poe vendor/tools/usr/bin no PATH.
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
V=$S/vendor
command -v unzip >/dev/null 2>&1 && exit 0
[ -x "$V/tools/usr/bin/unzip" ] && exit 0
T=$(mktemp -d /tmp/sodos-unzip-XXXXXX)
trap 'rm -rf "$T"' EXIT
(cd "$T" && apt-get download unzip)
mkdir -p "$V/tools"
dpkg -x "$T"/unzip_*.deb "$V/tools"
echo "unzip em $V/tools/usr/bin/unzip"
