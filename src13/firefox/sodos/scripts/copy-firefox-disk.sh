#!/bin/sh
# copy-firefox-disk.sh <disco> - copia o FIREFOX.EXE e dependencias para D:\FIREFOX.
#
# Chamado por "make disk-files". Sem build do browser, avisa e nao falha.
#
# Vai a arvore solta de dist/bin (chrome, modules, res, ...), com nomes longos:
# o build nao empacota o omni.ja. Os links simbolicos de dist/bin sao seguidos.
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

# mcopy nao segue links: monta uma copia de verdade e manda a arvore inteira.
T=$(mktemp -d /tmp/sodos-ffdisk-XXXXXX)
trap 'rm -rf "$T"' EXIT
(cd "$BIN" && tar -ch --exclude='*.elf' --exclude=FIREFOX.EXE --exclude=nsinstall \
   --exclude=pingsender --exclude='gmp-*' . 2>/dev/null) | tar -x -C "$T"
n=$(find "$T" -type f | wc -l)
if [ "$n" -gt 0 ]; then
  (cd "$T" && mcopy -s -o -Q -i "$DISK@@512" * ::/FIREFOX/) || exit 1
fi
echo "Firefox: FIREFOX.EXE e $n arquivos de recursos em D:\\FIREFOX."
[ "$n" -lt 5000 ] && echo "Firefox: poucos recursos; rode build-browser64.sh build misc e depois build libs."
exit 0
