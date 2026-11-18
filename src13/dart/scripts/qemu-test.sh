#!/bin/sh
# qemu-test.sh [-p porta] [-k] [-o SAIDA] [-g ARQ_NO_DISCO]... [-c COMANDO]... ARQ... [-- COMANDO...]
#
# Sobe o SODOS no QEMU numa COPIA do disk.img (o original nao muda), com os
# arquivos/pastas ARQ em D:\, e roda cada COMANDO pela API (POST /exec, o
# comando cru no corpo: a URL do httpd e curta), imprimindo a saida.
#   -c CMD   comando (repetivel, em ordem); "-- palavras" vira mais um no fim
#   -g ARQ   no fim, copia D:\ARQ do disco para SAIDA (padrao dart/build/qemu-out)
#   -k       deixa o QEMU de pe (a porta e o disco sao impressos)
#   -w       o AUTOEXEC tambem sobe o desktop (win), para /win/launch
#   -x CMD   roda CMD no host antes do boot, com DISK=<copia do disco> (ex.: copiar o Firefox)
# A serial vai para dart/build/qemu-serial.log (SERIAL_LOG=...).
#
#   sh dart/scripts/qemu-test.sh dart/build/EPOLLTST.EXE -- 'D:\EPOLLTST.EXE'
#
# Roda no WSL (usa os.img/disk*.img do src13). Uma linha do shell do SODOS tem
# no maximo 240 caracteres: argumentos longos vao num arquivo (@arquivo no dart2js).
set -e
S=$(cd "$(dirname "$0")/../.." && pwd)
PORT=8798
KEEP=
WIN=
PREP=
FILES=
GETS=
OUT=$S/dart/build/qemu-out
NL='
'
CMDS=
while [ $# -gt 0 ]; do
  case "$1" in
    -p) PORT=$2; shift 2 ;;
    -k) KEEP=1; shift ;;
    -w) WIN=1; shift ;;
    -x) PREP="$PREP$2$NL"; shift 2 ;;
    -o) OUT=$2; shift 2 ;;
    -g) GETS="$GETS $2"; shift 2 ;;
    -c) CMDS="$CMDS$2$NL"; shift 2 ;;
    --) shift; [ $# -gt 0 ] && CMDS="$CMDS$*$NL"; break ;;
    *) FILES="$FILES $1"; shift ;;
  esac
done

T=$(mktemp -d /tmp/sodos-dart-XXXXXX)
LOG=${SERIAL_LOG:-$S/dart/build/qemu-serial.log}
cleanup() {
  [ -n "$QPID" ] && [ -z "$KEEP" ] && kill "$QPID" 2>/dev/null
  [ -f "$T/serial.log" ] && mkdir -p "$(dirname "$LOG")" && cp "$T/serial.log" "$LOG"
  [ -z "$KEEP" ] && rm -rf "$T"
}
trap cleanup EXIT

cp --sparse=always "$S/disk.img" "$T/disk.img"
printf 'api start\r\n' > "$T/AUTOEXEC.BAT"
[ -n "$WIN" ] && printf 'win\r\n' >> "$T/AUTOEXEC.BAT"
if [ -n "$PREP" ]; then
  export DISK="$T/disk.img"
  printf '%s' "$PREP" | while IFS= read -r c; do
    [ -n "$c" ] && sh -c "$c"
  done
fi
mcopy -o -i "$T/disk.img@@512" "$T/AUTOEXEC.BAT" ::/AUTOEXEC.BAT
for f in $FILES; do
  if [ -d "$f" ]; then
    mcopy -s -o -Q -i "$T/disk.img@@512" "$f" ::/
  else
    mcopy -o -i "$T/disk.img@@512" "$f" "::/$(basename "$f" | tr a-z A-Z)"
  fi
done

ACCEL=tcg
[ -r /dev/kvm ] && [ -w /dev/kvm ] && grep -qE 'vmx|svm' /proc/cpuinfo && ACCEL=kvm
CPU=max
[ $ACCEL = kvm ] && CPU=host
# stdout/stderr do QEMU vao para o log: com -k ele fica vivo e nao pode
# segurar o pipe de quem chamou; setsid o tira da sessao (o fim do "wsl -e"
# manda SIGHUP).
setsid qemu-system-x86_64 -accel $ACCEL -cpu $CPU -m 8192 -smp ${SMP:-4} -usb -device usb-tablet \
  -vga std -display none -serial file:"$T/serial.log" \
  -netdev user,id=n0,hostfwd=tcp::$PORT-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
  -drive format=raw,file="$S/os.img",if=ide,index=0,snapshot=on \
  -drive format=raw,file="$T/disk.img",if=ide,index=1 \
  -drive format=raw,file="$S/disk2.img",if=ide,index=2,snapshot=on >"$T/qemu.log" 2>&1 </dev/null &
QPID=$!

i=0
until curl -s -m 2 "http://127.0.0.1:$PORT/win" >/dev/null 2>&1; do
  i=$((i + 1))
  if [ $i -gt 60 ]; then
    echo "qemu-test: a API nao respondeu em 60 s (serial em $T/serial.log)" >&2
    tail -20 "$T/serial.log" >&2
    exit 1
  fi
  sleep 1
done

RC=0
printf '%s' "$CMDS" | while IFS= read -r c; do
  [ -z "$c" ] && continue
  echo "## $c"
  t0=$(date +%s)
  curl -s -m ${TIMEOUT:-600} --data-binary "$c" "http://127.0.0.1:$PORT/exec" || echo "qemu-test: curl falhou ($?)"
  echo "## ($(( $(date +%s) - t0 )) s)"
done

if [ -n "$GETS" ]; then
  # o SODOS grava direto no disco: da para ler a copia com a VM de pe
  mkdir -p "$OUT"
  for g in $GETS; do
    mcopy -o -i "$T/disk.img@@512" "::/$(echo "$g" | tr '\\' /)" "$OUT/" && echo "qemu-test: $g -> $OUT/"
  done
fi
if [ -n "$KEEP" ]; then
  echo "qemu-test: QEMU pid $QPID, API em http://127.0.0.1:$PORT, disco em $T/disk.img"
  trap - EXIT
fi
exit $RC
