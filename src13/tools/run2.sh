#!/bin/bash
# run2.sh - QEMU com o SODOS do PC (a particao SODOSHD, E: no Windows) como D:
# (make run2). Feito em 2026-11-19.
#
# O QEMU do WSL nao abre a particao do SSD direto (precisa de admin e
# arriscaria o SODOS que roda no PC). Entao o E: e copiado para uma imagem de
# disco FAT32 (MBR, particao tipo 0x0C em 1 MiB), que o kernel monta como D:
# pelo FAT32.DLL do SODOS.PAK (drives_automount). O que o SODOS gravar no
# QEMU fica so na imagem; o E: nao muda.
#
#   make run2              copia so o que mudou no E: desde a ultima vez
#   make run2 RESYNC=1     refaz a imagem do zero (apagados no E: somem)
#   E2_DIR=/mnt/x          outra pasta no lugar do E:
#   E2_IMG=caminho         onde fica a imagem (padrao ~/sodos-run2/e.img)
#   E2_TAM=8G              tamanho da imagem (esparsa)
#
# Uso: tools/run2.sh "<comando do qemu>" (o Makefile passa o $(QEMU)).
set -e
QEMU_CMD=${1:-qemu-system-x86_64 -accel kvm -m 8192}
SRC=$(cd "$(dirname "$0")/.." && pwd)
DIR=${E2_DIR:-/mnt/e}
IMG=${E2_IMG:-$HOME/sodos-run2/e.img}
TAM=${E2_TAM:-8G}
STAMP=$IMG.sync
I="$IMG@@1M"

if [ ! -d "$DIR/BOOT" ] && [ ! -d "$DIR/SODOS" ]; then
  echo "run2: $DIR nao parece o SODOSHD (sem BOOT/ nem SODOS/). Monte o E: no WSL ou use E2_DIR=."
  exit 1
fi
for t in mcopy mmd mformat sfdisk mkfs.fat; do
  command -v $t >/dev/null 2>&1 || { echo "run2: falta $t (apt install mtools fdisk dosfstools)"; exit 1; }
done
mkdir -p "$(dirname "$IMG")"

# arquivos e pastas do Windows que nao vao para a imagem
pula() {
  case "$1" in
    '$RECYCLE.BIN'|'System Volume Information'|Desktop.ini|desktop.ini) return 0 ;;
  esac
  return 1
}

if [ "${RESYNC:-0}" = 1 ] || [ ! -f "$IMG" ]; then
  echo "run2: criando $IMG ($TAM, FAT32) a partir de $DIR..."
  rm -f "$IMG" "$STAMP"
  truncate -s "$TAM" "$IMG"
  echo 'start=2048, type=c' | sfdisk -q "$IMG"
  mkfs.fat -F 32 -n SODOSHD --offset 2048 "$IMG" >/dev/null
  cd "$DIR"
  for f in * .[!.]*; do
    [ -e "$f" ] || continue
    pula "$f" && continue
    printf '  %s\n' "$f"
    mcopy -s -o -Q -i "$I" "$f" ::/ 2>/dev/null || echo "run2: aviso: $f nao copiou inteiro"
  done
  cd - >/dev/null
  touch "$STAMP"
else
  # so o que mudou desde a ultima copia (arquivos novos ou alterados)
  n=0
  cd "$DIR"
  while IFS= read -r -d '' f; do
    f=${f#./}
    top=${f%%/*}
    pula "$top" && continue
    d=$(dirname "$f")
    if [ "$d" != . ]; then
      # cria as pastas no caminho, uma a uma
      p=""
      IFS=/ read -r -a partes <<< "$d"
      for q in "${partes[@]}"; do
        p=${p:+$p/}$q
        mmd -i "$I" "::/$p" 2>/dev/null || true
      done
    fi
    mcopy -o -Q -i "$I" "$f" "::/$f" 2>/dev/null || echo "run2: aviso: $f nao copiou"
    n=$((n + 1))
  done < <(find . -type f -newer "$STAMP" -print0 2>/dev/null)
  cd - >/dev/null
  touch "$STAMP"
  echo "run2: $n arquivo(s) atualizados de $DIR (RESYNC=1 refaz tudo)."
fi

cd "$SRC"
echo "run2: D: = copia do $DIR; API em http://localhost:8797"
# disk2.img com snapshot=on: outro QEMU (a sessao do WINE) pode estar com ele
exec $QEMU_CMD -vga std -netdev user,id=n0,hostfwd=tcp::8797-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
  -drive format=raw,file=os.img,if=ide,index=0 \
  -drive format=raw,file="$IMG",if=ide,index=1 \
  -drive format=raw,file=disk2.img,if=ide,index=2,snapshot=on
