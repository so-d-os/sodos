#!/bin/bash
set -e
mkdir -p /mnt/c/sodos/fonts
cp -n /mnt/c/Windows/Fonts/SansSerifCollection.ttf /mnt/c/sodos/fonts/SansSerifCollection.ttf
cd /tmp
urls=(
  "https://github.com/notofonts/latin-greek-cyrillic/releases/download/NotoSans-v2.015/NotoSans-v2.015.zip"
)
# try raw github paths one by one
try() {
  echo "GET $1 -> $2"
  if curl -fL --retry 2 -o "$2" "$1"; then
    file "$2" || true
    ls -l "$2"
  else
    echo FAIL "$1"
  fi
}
try "https://github.com/googlefonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Regular.ttf" /mnt/c/sodos/fonts/NotoSans-Regular.ttf
try "https://github.com/googlefonts/noto-fonts/raw/main/hinted/ttf/NotoSerif/NotoSerif-Regular.ttf" /mnt/c/sodos/fonts/NotoSerif-Regular.ttf
try "https://github.com/googlefonts/noto-fonts/raw/main/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf" /mnt/c/sodos/fonts/NotoSansMono-Regular.ttf
ls -l /mnt/c/sodos/fonts
