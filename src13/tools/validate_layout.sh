#!/bin/sh
# validate_layout.sh - valida tamanhos, enderecos e sobreposicoes do boot.
#
# Uso:
#   validate_layout.sh kernel.bin kernel.elf stage2.bin app.bin
#
# Constantes vem do Makefile por variaveis de ambiente.  O script e chamado
# pelo alvo check-layout antes de gerar os.img; qualquer inconsistencia falha
# o build com uma mensagem explicita.
set -u
LC_ALL=C
export LC_ALL

KERNEL_BIN=${1:-kernel.bin}
KERNEL_ELF=${2:-kernel.elf}
STAGE2_BIN=${3:-stage2.bin}
APP_BIN=${4:-app.bin}

KERNEL_LOAD=${KERNEL_LOAD:-0x01000000}
KERNEL_MAX_SIZE=${KERNEL_MAX_SIZE:-0x0E000000}
APP_LOAD=${APP_LOAD:-0x00058000}
APP_MAX_SIZE=${APP_MAX_SIZE:-0x00008000}
STAGE2_LOAD=${STAGE2_LOAD:-0x00007E00}
STAGE2_MAX_SIZE=${STAGE2_MAX_SIZE:-0x00007000}
BOOTINFO_ADDR=${BOOTINFO_ADDR:-0x00005000}
E820_ADDR=${E820_ADDR:-0x00005200}
IMAGE_MAX_SECTORS=${IMAGE_MAX_SECTORS:-1048576}

die() {
  echo "layout: erro: $*" >&2
  exit 1
}

dec() {
  case "$1" in
    0x*|0X*) printf '%d' "$1" ;;
    *)       printf '%d' "0x$1" ;;
  esac
}

file_size() {
  wc -c < "$1" | tr -d ' \r\n'
}

sectors() {
  echo $(( ($1 + 511) / 512 ))
}

# overlap inicio1 fim1 inicio2 fim2  (fim exclusivo)
overlap() {
  [ "$1" -lt "$4" ] && [ "$3" -lt "$2" ]
}

for f in "$KERNEL_BIN" "$KERNEL_ELF" "$STAGE2_BIN" "$APP_BIN"; do
  [ -f "$f" ] || die "arquivo ausente: $f"
done

KLOAD=$(dec "$KERNEL_LOAD")
KMAX=$(dec "$KERNEL_MAX_SIZE")
ALOAD=$(dec "$APP_LOAD")
AMAX=$(dec "$APP_MAX_SIZE")
S2LOAD=$(dec "$STAGE2_LOAD")
S2MAX=$(dec "$STAGE2_MAX_SIZE")
BIADDR=$(dec "$BOOTINFO_ADDR")
E820ADDR=$(dec "$E820_ADDR")

KERNEL_SIZE=$(file_size "$KERNEL_BIN")
STAGE2_SIZE=$(file_size "$STAGE2_BIN")
APP_SIZE=$(file_size "$APP_BIN")
KERNEL_SECTORS=$(sectors "$KERNEL_SIZE")
STAGE2_SECTORS=$(sectors "$STAGE2_SIZE")
APP_SECTORS=$(sectors "$APP_SIZE")

[ "$KERNEL_SIZE" -gt 0 ] || die "kernel.bin vazio"
[ "$STAGE2_SIZE" -gt 0 ] || die "stage2.bin vazio"
[ "$STAGE2_SIZE" -le "$S2MAX" ] || die "stage2.bin=$STAGE2_SIZE excede STAGE2_MAX_SIZE=$S2MAX"
[ "$STAGE2_SECTORS" -le 56 ] || die "stage2 ocupa $STAGE2_SECTORS setores; maximo 56"
[ "$APP_SIZE" -le "$AMAX" ] || die "app.bin=$APP_SIZE excede APP_MAX_SIZE=$AMAX"
[ "$APP_SIZE" -gt 0 ] || die "app.bin vazio"

BSS_END_HEX=$(nm "$KERNEL_ELF" | awk '$3 == "__bss_end" { print $1; exit }')
[ -n "$BSS_END_HEX" ] || die "__bss_end ausente em kernel.elf"
BSS_END=$(dec "0x$BSS_END_HEX")

ENTRY_HEX=$(nm "$KERNEL_ELF" | awk '$3 == "kernel_entry" { print $1; exit }')
[ -n "$ENTRY_HEX" ] || die "kernel_entry ausente em kernel.elf"
KENTRY=$(dec "0x$ENTRY_HEX")

KERNEL_RESERVED=$KERNEL_SIZE
if [ "$BSS_END" -gt $(( KLOAD + KERNEL_SIZE )) ]; then
  KERNEL_RESERVED=$(( BSS_END - KLOAD ))
fi
if [ $(( KERNEL_SECTORS * 512 )) -gt "$KERNEL_RESERVED" ]; then
  KERNEL_RESERVED=$(( KERNEL_SECTORS * 512 ))
fi

# Faixa do kernel.
[ "$KLOAD" -ge 1048576 ] || die "KERNEL_LOAD=0x$(printf '%x' "$KLOAD") precisa ficar acima de 1 MiB"
[ $(( KLOAD % 1048576 )) -eq 0 ] || die "KERNEL_LOAD precisa ser multiplo de 1 MiB"
[ "$KLOAD" -ge 16777216 ] || die "KERNEL_LOAD precisa ser >= 0x01000000 (tela/FAT/heap vivem abaixo)"
[ "$KERNEL_RESERVED" -le "$KMAX" ] || die "kernel reserva $KERNEL_RESERVED bytes; KERNEL_MAX_SIZE=$KMAX"
[ $(( KLOAD + KERNEL_RESERVED )) -le 268435456 ] || die "kernel ultrapassa a identidade de 256 MiB"
[ "$KENTRY" -ge "$KLOAD" ] || die "kernel_entry=0x$(printf '%x' "$KENTRY") antes de KERNEL_LOAD"
[ "$KENTRY" -lt $(( KLOAD + KERNEL_RESERVED )) ] || die "kernel_entry fora da faixa reservada do kernel"
[ "$BSS_END" -ge "$KLOAD" ] || die "__bss_end antes de KERNEL_LOAD"

# Segmentos ELF: nenhuma secao pode ficar abaixo da base nem fora da reserva.
LOADS_TMP=".layout-loads.$$"
trap 'rm -f "$LOADS_TMP"' EXIT HUP INT TERM
readelf -lW "$KERNEL_ELF" | awk '$1 == "LOAD" { print $3, $6 }' > "$LOADS_TMP"
[ -s "$LOADS_TMP" ] || die "kernel.elf nao tem segmento LOAD"
MIN_LOAD=0
MAX_LOAD=0
FIRST=1
while read -r VADDR MEMSIZ; do
  V=$(dec "$VADDR")
  M=$(dec "$MEMSIZ")
  if [ "$FIRST" -eq 1 ]; then
    MIN_LOAD=$V
    MAX_LOAD=$V
    FIRST=0
  fi
  [ "$V" -lt "$MIN_LOAD" ] && MIN_LOAD=$V
  [ $(( V + M )) -gt "$MAX_LOAD" ] && MAX_LOAD=$(( V + M ))
done < "$LOADS_TMP"
[ "$MIN_LOAD" -eq "$KLOAD" ] || die "primeiro LOAD=0x$(printf '%x' "$MIN_LOAD") difere de KERNEL_LOAD=0x$(printf '%x' "$KLOAD")"
[ "$MAX_LOAD" -le $(( KLOAD + KERNEL_RESERVED )) ] || die "LOAD termina em 0x$(printf '%x' "$MAX_LOAD") fora da reserva do kernel"

# O buffer de formatacao deve ser simbolo absoluto em 0x80800, nao secao
# carregavel (era isso que fazia kernel.bin atravessar o slot da aplicacao).
FB=$(nm "$KERNEL_ELF" | awk '$3 == "format_buf_region" { print $2, $1; exit }')
[ -n "$FB" ] || die "format_buf_region ausente em kernel.elf"
set -- $FB
[ "$1" = "A" ] || die "format_buf_region deve ser absoluto (tipo A), nao carregado (tipo $1)"
[ "$(dec "0x$2")" -eq 526336 ] || die "format_buf_region deve ficar em 0x80800"

# Aplicacao/initrd no slot legado.
[ "$ALOAD" -ge 65536 ] || die "APP_LOAD abaixo de 64 KiB"
[ $(( ALOAD + APP_SECTORS * 512 )) -le 393216 ] || die "app ultrapassa 0x60000"
[ $(( ALOAD + APP_SIZE )) -le 393216 ] || die "app.bin ultrapassa 0x60000"

# Estruturas baixas do bootloader.
[ "$S2LOAD" -eq 32256 ] || die "STAGE2_LOAD deve ser 0x7E00"
[ $(( S2LOAD + STAGE2_SIZE )) -le $(( S2LOAD + S2MAX )) ] || die "stage2 fora do buffer"
[ $(( BIADDR + 128 )) -le "$E820ADDR" ] || die "BootInfo sobrepoe o mapa E820"
[ $(( E820ADDR + 64 * 24 )) -le 24576 ] || die "mapa E820 ultrapassa 0x6000 (page tables do kernel)"
[ "$BIADDR" -ge 20480 ] || die "BootInfo abaixo de 0x5000"

# Sobreposicoes entre componentes carregados.
S1_START=31744
S1_END=32256
S2_START=$S2LOAD
S2_END=$(( S2LOAD + STAGE2_SIZE ))
BI_END=$(( BIADDR + 128 ))
E820_END=$(( E820ADDR + 64 * 24 ))
APP_START=$ALOAD
APP_END=$(( ALOAD + APP_SIZE ))
K_START=$KLOAD
K_END=$(( KLOAD + KERNEL_RESERVED ))

overlap "$S1_START" "$S1_END" "$S2_START" "$S2_END" && die "stage1 sobrepoe stage2"
overlap "$S2_START" "$S2_END" "$BIADDR" "$BI_END" && die "stage2 sobrepoe BootInfo"
overlap "$S2_START" "$S2_END" "$E820ADDR" "$E820_END" && die "stage2 sobrepoe mapa E820"
overlap "$S2_START" "$S2_END" "$APP_START" "$APP_END" && die "stage2 sobrepoe aplicacao"
overlap "$S2_START" "$S2_END" "$K_START" "$K_END" && die "stage2 sobrepoe kernel"
overlap "$BIADDR" "$BI_END" "$E820ADDR" "$E820_END" && die "BootInfo sobrepoe mapa E820"
overlap "$BIADDR" "$BI_END" "$APP_START" "$APP_END" && die "BootInfo sobrepoe aplicacao"
overlap "$BIADDR" "$BI_END" "$K_START" "$K_END" && die "BootInfo sobrepoe kernel"
overlap "$E820ADDR" "$E820_END" "$APP_START" "$APP_END" && die "mapa E820 sobrepoe aplicacao"
overlap "$E820ADDR" "$E820_END" "$K_START" "$K_END" && die "mapa E820 sobrepoe kernel"
overlap "$APP_START" "$APP_END" "$K_START" "$K_END" && die "aplicacao sobrepoe kernel"

TOTAL_SECTORS=$(( 1 + STAGE2_SECTORS + KERNEL_SECTORS + APP_SECTORS ))
[ "$TOTAL_SECTORS" -le "$IMAGE_MAX_SECTORS" ] || die "imagem com $TOTAL_SECTORS setores excede $IMAGE_MAX_SECTORS"

echo "layout: OK"
echo "layout: stage1  0x7C00 (512 bytes)"
echo "layout: stage2  0x$(printf '%x' "$S2_START") size=$STAGE2_SIZE sectors=$STAGE2_SECTORS"
echo "layout: kernel  0x$(printf '%x' "$K_START") size=$KERNEL_SIZE reserved=$KERNEL_RESERVED sectors=$KERNEL_SECTORS entry=0x$(printf '%x' "$KENTRY")"
echo "layout: app     0x$(printf '%x' "$APP_START") size=$APP_SIZE sectors=$APP_SECTORS"
echo "layout: bootinfo 0x$(printf '%x' "$BIADDR") e820 0x$(printf '%x' "$E820ADDR")"
echo "layout: image   $TOTAL_SECTORS sectors"
