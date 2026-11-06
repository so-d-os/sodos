#!/bin/sh
# copy-firefox-disk.sh <disco> - copia o FIREFOX.EXE e dependencias para D:\FIREFOX.
#
# Chamado por "make disk-files". Sem build do browser, avisa e nao falha.
#
# So entram os nomes que cabem em 8.3: o kernel nao le nomes longos do FAT. Os
# demais sao listados no final; eles so vao funcionar com o omni.ja empacotado.
DISK=$(realpath "$1")  # o script muda de diretorio
O=${FIREFOX_OBJ:-$HOME/ffsrc/obj-browser64}
BIN=$O/dist/bin
EXE=$BIN/FIREFOX.EXE

if [ ! -f "$EXE" ]; then
  echo "Firefox: $EXE nao existe; rode sodos/scripts/link-firefox64.sh. Pulando."
  exit 0
fi

need=$(( $(stat -L -c %s "$EXE") + 64 * 1024 * 1024 ))
have=$(stat -L -c %s "$DISK")
if [ "$have" -lt "$need" ]; then
  echo "Firefox: $DISK ($((have / 1048576)) MB) nao comporta o FIREFOX.EXE; rode make grow-disk. Pulando."
  exit 0
fi

mkdir_d() {
  mdir -i "$DISK@@512" "::/$1" >/dev/null 2>&1 || mmd -i "$DISK@@512" "::/$1" || exit 1
}

mkdir_d FIREFOX
mcopy -o -i "$DISK@@512" "$EXE" ::/FIREFOX/FIREFOX.EXE || exit 1

fits_83() {
  case "$1" in
    .*|*.*.*) return 1 ;;
  esac
  b=${1%%.*}
  e=
  case "$1" in *.*) e=${1#*.} ;; esac
  [ ${#b} -ge 1 ] && [ ${#b} -le 8 ] && [ ${#e} -le 3 ] || return 1
  case "$1" in *[!A-Za-z0-9_.-]*) return 1 ;; esac
  return 0
}

copied=1
skipped=0
skipped_list=$(mktemp)
cd "$BIN" || exit 1
for f in $(find -L . -type f ! -name FIREFOX.EXE ! -name '*.elf' ! -name nsinstall ! -name pingsender \
           ! -name '.*' ! -path './gmp-*' | sed 's#^\./##'); do
  ok=1
  old_ifs=$IFS
  IFS=/
  for part in $f; do
    fits_83 "$part" || ok=0
  done
  IFS=$old_ifs
  if [ "$ok" = 0 ]; then
    skipped=$((skipped + 1))
    echo "$f" >> "$skipped_list"
    continue
  fi
  up=$(printf '%s' "$f" | tr 'a-z' 'A-Z')
  d=$(dirname "$up")
  if [ "$d" != "." ]; then
    p=FIREFOX
    old_ifs=$IFS
    IFS=/
    for part in $d; do
      p=$p/$part
      IFS=$old_ifs
      mkdir_d "$p"
      IFS=/
    done
    IFS=$old_ifs
  fi
  mcopy -o -i "$DISK@@512" "$f" "::/FIREFOX/$up" || exit 1
  copied=$((copied + 1))
done

echo "Firefox: $copied arquivos em D:\\FIREFOX; $skipped fora, com nome maior que 8.3."
if [ "$skipped" -gt 0 ]; then
  head -8 "$skipped_list" | sed 's/^/  fora: /'
fi
rm -f "$skipped_list"
