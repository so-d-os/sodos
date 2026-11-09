#!/bin/sh
# vm.sh - SODOS de teste, separado do QEMU de uso diario.
#
# Usa copias de os.img/disk.img/disk2.img em $BUILD_ROOT/vm (o disk.img copiado
# e esparso) e responde na porta 8798, sem janela. Assim da para gravar
# binarios no D: sem fechar o QEMU principal (porta 8797).
#
#   sh scripts/vm.sh init                 copia os discos (refaz com init -f)
#   sh scripts/vm.sh put ARQ [NOME]       grava ARQ em D:\NOME (para a VM antes)
#   sh scripts/vm.sh start                sobe a VM e espera a API HTTP
#   sh scripts/vm.sh exec 'D:\ZTEST.EXE'  roda um comando e mostra a saida
#   sh scripts/vm.sh mon "info registers"   monitor do QEMU de fora (depuracao)
#   sh scripts/vm.sh stop
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
. "$S/scripts/env.sh"
VM=$BUILD_ROOT/vm
PORT=${SODOS_VM_PORT:-8798}
PIDF=$VM/qemu.pid

running() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }

case "$1" in
  init)
    if [ -f "$VM/disk.img" ] && [ "$2" != "-f" ]; then echo "ja existe: $VM"; exit 0; fi
    mkdir -p "$VM"
    for f in os.img disk.img disk2.img; do cp --sparse=always "$SRC13/$f" "$VM/$f"; done
    echo "discos em $VM"
    ;;
  put)
    [ -f "$VM/disk.img" ] || sh "$0" init
    if running; then sh "$0" stop; fi
    N=${3:-$(basename "$2" | tr a-z A-Z)}
    # NOME pode ter diretorio (QEMU/bios.bin): cria o diretorio se faltar.
    case "$N" in
      */*) mmd -D s -i "$VM/disk.img@@512" "::/${N%/*}" 2>/dev/null || true ;;
    esac
    mcopy -o -i "$VM/disk.img@@512" "$2" "::/$N"
    printf 'D:\\%s\n' "$N"
    ;;
  start)
    [ -f "$VM/disk.img" ] || sh "$0" init
    if running; then echo "ja rodando"; exit 0; fi
    # KVM quando o usuario pode abrir /dev/kvm (grupo kvm) e a CPU tem
    # virtualizacao (no WSL: nestedVirtualization); senao TCG.
    ACCEL=tcg
    if [ -r /dev/kvm ] && [ -w /dev/kvm ] && grep -qE 'vmx|svm' /proc/cpuinfo; then ACCEL=kvm; fi
    qemu-system-x86_64 -accel $ACCEL -m 2048 -display none -vga std \
      -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:8797 -device ne2k_pci,netdev=n0 \
      -boot order=c -serial file:"$VM/serial.log" \
      -drive format=raw,file="$VM/os.img",if=ide,index=0 \
      -drive format=raw,file="$VM/disk.img",if=ide,index=1 \
      -drive format=raw,file="$VM/disk2.img",if=ide,index=2 \
      -monitor unix:"$VM/monitor.sock",server,nowait \
      -daemonize -pidfile "$PIDF"
    i=0
    while [ $i -lt 90 ]; do
      if curl -s -m 2 -X POST --data-binary 'ver' "http://127.0.0.1:$PORT/exec" >/dev/null 2>&1; then
        echo "VM pronta (porta $PORT)"; exit 0
      fi
      i=$((i + 1)); sleep 1
    done
    echo "VM nao respondeu; veja $VM/serial.log"; exit 1
    ;;
  exec)
    shift
    curl -s -m "${SODOS_VM_TIMEOUT:-120}" -X POST --data-binary "$*" "http://127.0.0.1:$PORT/exec"
    ;;
  mon)
    # Comando no monitor do QEMU de fora (o que roda o SODOS), ex.:
    #   sh scripts/vm.sh mon 'info registers'
    shift
    python3 - "$VM/monitor.sock" "$*" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.settimeout(1.0)
def drain():
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
    except socket.timeout:
        pass
    return out
drain()
s.sendall(sys.argv[2].encode() + b"\n")
time.sleep(0.3)
sys.stdout.write(drain().decode(errors="replace"))
PY
    ;;
  stop)
    if running; then kill "$(cat "$PIDF")"; sleep 1; fi
    rm -f "$PIDF"
    ;;
  *) sed -n 2,14p "$0"; exit 1 ;;
esac
