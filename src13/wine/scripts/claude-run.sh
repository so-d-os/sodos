#!/bin/sh
# claude-run.sh - bancada do Claude Desktop no WINE (plano F0, docs/05).
#
#   sh wine/scripts/claude-run.sh [segundos]          (padrao 240)
#   CLAUDE_ARGS="..." sh wine/scripts/claude-run.sh    (troca as opcoes)
#
# Recompila o WINE.DLL, sobe a VM persistente se nao estiver no ar
# (VMDIR=~/sodos-wine-vm, que ja tem o Claude instalado), troca o WINE.DLL por
# FETCH.EXE sem reiniciar (o host serve wine/build na porta 8810 e a VM le de
# 10.0.2.2), roda o Claude.exe com WINE_ARGS e junta em wine/build/claude-run/:
#   report.txt  linhas FATAL/ERROR do Chromium, excecao com RVA, processos
#               filhos, APIs ausentes chamadas, ExitProcess e quem chamou
#   tela.png    captura pelo monitor do QEMU (a API HTTP fica ocupada)
#   serial.txt  a serial desta rodada
S=$(cd "$(dirname "$0")/../.." && pwd)
export VMDIR=${VMDIR:-$HOME/sodos-wine-vm}
export PORT=${PORT:-8803}
V=$VMDIR
T=${1:-240}
OUT=$S/wine/build/claude-run
APP='D:\WINE\APPS\Claude_5.11492.5.0_x64__pzs8sxrjxfjjc\app\Claude.exe'
# processos separados (--single-process da CHECK na partida do Electron 45, docs/05 F1)
ARGS=${CLAUDE_ARGS:---no-sandbox --disable-gpu --disable-software-rasterizer --use-gl=disabled --disable-3d-apis --disable-webgl --disable-webgpu --disable-accelerated-2d-canvas --disable-gpu-rasterization --ipc-connection-timeout=300 --enable-logging=stderr --v=0}
mkdir -p "$OUT"
cd "$S" || exit 1

make -C wine build/WINE.DLL > "$OUT/build.log" 2>&1 || { grep -n "error" "$OUT/build.log" | head; exit 1; }

# servidor HTTP do host com wine/build
if ! curl -s -m 2 -o /dev/null http://127.0.0.1:8810/WINE.DLL; then
  # so no loopback: a rede do QEMU (10.0.2.2) chega no 127.0.0.1 do host
  (cd wine/build && setsid python3 -m http.server 8810 --bind 127.0.0.1 >/dev/null 2>&1 < /dev/null &)
  sleep 1
fi

# VM no ar?
if ! curl -s -m 3 -o /dev/null "http://127.0.0.1:$PORT/win"; then
  KEEP=1 sh wine/scripts/vm.sh start win || exit 1
  rm -f "$V/wine.md5"
fi
cmd() { TIMEOUT=${2:-120} sh wine/scripts/vm.sh cmd "$1"; }

# WINE.DLL novo (so se mudou): baixa, desliga o WINE para soltar o antigo
NEW=$(md5sum wine/build/WINE.DLL | cut -d" " -f1)
if [ "$NEW" != "$(cat "$V/wine.md5" 2>/dev/null)" ]; then
  cmd "SET WINE=1" >/dev/null
  cmd 'D:\FETCH.EXE http://10.0.2.2:8810/WINE.DLL D:\SODOS\DRV\WINE.DLL' | tail -1
  cmd "SET WINE=0" >/dev/null
  echo "$NEW" > "$V/wine.md5"
fi
cmd "SET WINE=1" >/dev/null
cmd "SET WINEDEBUG=${WINEDEBUG-2}" >/dev/null
cmd "SET HIMEM=${HIMEM:-80}" >/dev/null
cmd "SET ELECTRON_ENABLE_LOGGING=1" >/dev/null
cmd "SET WINE_ARGS_EXE=app\claude.exe" >/dev/null
# "Abrir com": o login abre a URL no navegador do LINKEXT.CFG (HTTP,HTTPS=...)
cmd "SET LINKEXT=1" >/dev/null
# APPDATA=ram: AppData do perfil no ramdisk Z: (cache do Chromium sem ATA/BKL);
# vazio = no D: (persistente)
cmd "SET WINEAPPDATA=${APPDATA_DIR-Z:\WINEAPP}" >/dev/null
# o perfil do Z: (login) e salvo num arquivo do D: e volta no proximo boot
# (profsave.c); APPSAVE= desliga
cmd "SET WINEAPPSAVE=${APPSAVE-D:\WINEAPP.SAV}" >/dev/null
# o SET do SODOS guarda ate 259 caracteres: divide em WINE_ARGS, WINE_ARGS2, WINE_ARGS3
set -- $ARGS
A1=""; A2=""; A3=""
for a in "$@"; do
  if [ $(( ${#A1} + ${#a} + 1 )) -lt 200 ] && [ -z "$A2" ]; then A1="${A1:+$A1 }$a"
  elif [ $(( ${#A2} + ${#a} + 1 )) -lt 200 ] && [ -z "$A3" ]; then A2="${A2:+$A2 }$a"
  else A3="${A3:+$A3 }$a"; fi
done
cmd "SET WINE_ARGS=$A1" >/dev/null
cmd "SET WINE_ARGS2=$A2" >/dev/null
cmd "SET WINE_ARGS3=$A3" >/dev/null

N=$(wc -l < "$V/serial.log")
# pelo WIN (tarefa propria): o laco do WIN fica livre para abrir os processos filhos
sh wine/scripts/vm.sh launch "$APP" > "$OUT/stdout.txt" 2>&1
i=0
while [ $i -lt "$T" ]; do
  sleep 5
  i=$((i + 5))
  # o processo principal terminou (o primeiro "start" e o do Claude)
  tail -n +"$N" "$V/serial.log" | grep -aq "\[wine\] ExitProcess" && break
done
printf "screendump /tmp/claude-run.ppm\n" | python3 "$S/wine/scripts/qmon.py" "$V/mon" > /dev/null 2>&1
sleep 2
python3 "$S/wine/scripts/ppm2png.py" /tmp/claude-run.ppm "$OUT/tela.png" > /dev/null 2>&1
tail -n +"$N" "$V/serial.log" > "$OUT/serial.txt"

{
  echo "== Claude.exe ($i s) opcoes: $ARGS"
  echo "== saida do programa (FATAL/ERROR/WARNING)"
  grep -a "saida: " "$OUT/serial.txt" | grep -a "FATAL\|ERROR\|WARNING\|Check failed" | sed 's/.*saida: //' | head -20
  echo "== excecao"
  grep -a -A3 "excecao nao tratada" "$OUT/serial.txt" | head -8
  grep -a "  pilha: " "$OUT/serial.txt" | head -1
  echo "== processos filhos"
  grep -a "processo filho" "$OUT/serial.txt" | head
  echo "== APIs ausentes chamadas"
  grep -a "API sem implementacao\|funcao nao implementada\|ntdll sem implementacao\|GetProcAddress: falta\|ordinal falhou" "$OUT/serial.txt" | sed 's/.*\] //' | sort | uniq -c | sort -rn | head -25
  echo "== fim"
  grep -a "espera longa: ExitProcess\|\[wine\] ExitProcess\|pilha: ExitProcess" "$OUT/serial.txt" | head -3
} > "$OUT/report.txt"
cat "$OUT/report.txt"
echo "(tela em wine/build/claude-run/tela.png)"
