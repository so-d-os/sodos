#!/bin/sh
# install/install.sh - instala os componentes do SODOS no D:
#
#   sh install/install.sh [-n] <disco> [componente...]
#
#   <disco>      disk.img (imagem FAT do QEMU, gravada com o mtools) ou a pasta
#                onde a FAT32 do D: esta montada (SSD: make send, make sendpc)
#   componente   nome de install/componentes/<nome>.lst (sem o .lst) ou o
#                caminho de um .lst; sem nenhum, todos, em ordem alfabetica
#   -n           so mostra o que faria, sem gravar nada
#
# O "make disk-files" chama este script (e com ele o make send e o sendpc).
# O formato das listas esta em install/README.md.
set -u

AQUI=$(cd "$(dirname "$0")" && pwd)
RAIZ=$(cd "$AQUI/.." && pwd)
SECO=0
[ "${1:-}" = -n ] && { SECO=1; shift; }
[ $# -ge 1 ] || { sed -n '2,13p' "$0"; exit 2; }

DISCO=$1; shift
[ -e "$DISCO" ] || { echo "install: $DISCO nao existe"; exit 1; }
DISCO=$(cd "$(dirname "$DISCO")" && pwd)/$(basename "$DISCO")
IMG="$DISCO@@512"

if [ -f "$DISCO" ] && pgrep -f "[q]emu-system.*file=[^,]*$(basename "$DISCO")" >/dev/null 2>&1; then
  echo "install: feche o QEMU antes: o SO regrava a FAT e apaga o que o mcopy gravou."
  exit 1
fi
# Pasta montada: o mcopy/mmd/mdir do tools/dirfs copia direto nela (so o que
# mudou). O make send e o sendpc ja poem o dirfs no PATH; rodando a mao, aqui.
if [ -d "$DISCO" ] && ! mdir -i "$IMG" ::/ >/dev/null 2>&1; then
  PATH="$RAIZ/src13/tools/dirfs:$PATH"
  mdir -i "$IMG" ::/ >/dev/null 2>&1 || { echo "install: o tools/dirfs nao le $DISCO"; exit 1; }
fi
if [ -f "$DISCO" ] && ! mdir -i "$IMG" ::/ >/dev/null 2>&1; then
  echo "install: $DISCO nao tem FAT no setor 1 (make format-disk?)"
  exit 1
fi

# Um .lst dado pelo caminho vale a partir da pasta de quem chamou.
k=$#
for c in "$@"; do
  case "$c" in */*|*.lst) [ -f "$c" ] && c=$(cd "$(dirname "$c")" && pwd)/$(basename "$c") ;; esac
  set -- "$@" "$c"
done
shift $k

cd "$RAIZ" || exit 1  # as origens das listas sao relativas a raiz do repositorio
CR=$(printf '\r')
ERROS=0
FEITAS="|"  # pastas que ja existem no disco (cache do mdir)
TMPF=$(mktemp)
trap 'rm -f "$TMPF"' EXIT

erro()  { echo "install: $ONDE: ERRO: $*"; ERROS=$((ERROS + 1)); }
aviso() { echo "install: $COMP: $*"; }

# D:\A\B -> ::/A/B (o \ do fim fica: D:\A\ -> ::/A/). So o D: e o disco.
para_mtools() {
  case "$1" in
    [Dd]:\\*) printf '::%s' "$(printf '%s' "${1#??}" | tr '\\' '/')" ;;
    *) return 1 ;;
  esac
}

# Cria ::/A, ::/A/B... o que faltar.
garante_pasta() {
  resto=${1#::}; resto=${resto#/}; atual=::
  while [ -n "$resto" ]; do
    parte=${resto%%/*}
    case "$resto" in */*) resto=${resto#*/} ;; *) resto= ;; esac
    [ -n "$parte" ] || continue
    atual="$atual/$parte"
    case "$FEITAS" in *"|$atual|"*) continue ;; esac
    if ! mdir -i "$IMG" "$atual" </dev/null >/dev/null 2>&1; then
      if [ $SECO = 1 ]; then echo "  md   $atual"
      else mmd -i "$IMG" "$atual" </dev/null >/dev/null || return 1
      fi
    fi
    FEITAS="$FEITAS$atual|"
  done
}

# Nome no D: em maiusculas (8.3, como o DOS mostra), a nao ser com nome-original.
nome_d() {
  if [ $original = 1 ]; then printf '%s' "$1"; else printf '%s' "$1" | tr 'a-z' 'A-Z'; fi
}

# 0 se o nome casa com algum exceto=
excluido() {
  set -f; r=1
  for p in $excetos; do case "$1" in $p) r=0 ;; esac; done
  set +f; return $r
}

# poe <arquivo> <destino mtools completo>
poe() {
  garante_pasta "${2%/*}" || { erro "nao criei a pasta ${2%/*}"; return 1; }
  if [ $preserva = 1 ] && mdir -i "$IMG" "$2" </dev/null >/dev/null 2>&1; then
    PRESERVADOS=$((PRESERVADOS + 1))
    return 0
  fi
  if [ $SECO = 1 ]; then echo "  copy $1 -> $2"
  else mcopy -o -i "$IMG" "$1" "$2" </dev/null || { erro "mcopy $1 -> $2"; return 1; }
  fi
  COPIADOS=$((COPIADOS + 1))
}

# copia <origem> <destino> [opcoes] [| mensagem]; opcoes no README.md
faz_copia() {
  orig=${1%/}; dest=$2; omsg=$3; shift 3
  opcional=0; preserva=0; arvore=0; original=0; excetos=
  for o in "$@"; do
    case "$o" in
      opcional) opcional=1 ;;
      preserva) preserva=1 ;;
      arvore) arvore=1 ;;
      nome-original) original=1 ;;
      exceto=?*) excetos="$excetos ${o#exceto=}" ;;
      *) erro "opcao desconhecida: $o"; return ;;
    esac
  done
  t=$(para_mtools "$dest") || { erro "destino fora do D: $dest"; return; }
  case "$t" in
    */) ;;
    *) [ $arvore = 0 ] || { erro "com arvore o destino e uma pasta (termina em \\)"; return; } ;;
  esac
  achou=0
  if [ $arvore = 1 ]; then
    if [ -d "$orig" ]; then
      achou=1  # pasta vazia nao e falta
      find "$orig" -type f | LC_ALL=C sort >"$TMPF"
      while IFS= read -r f <&4; do
        excluido "${f##*/}" && continue
        poe "$f" "$t$(nome_d "${f#"$orig"/}")" || return
      done 4<"$TMPF"
    fi
  else
    for f in $orig; do
      [ -f "$f" ] || continue
      excluido "${f##*/}" && continue
      achou=1
      case "$t" in */) u="$t$(nome_d "${f##*/}")" ;; *) u=$t ;; esac
      poe "$f" "$u" || return
    done
  fi
  if [ $achou = 0 ]; then
    if [ $opcional = 1 ]; then aviso "${omsg:-falta $orig (nao compilado?): $dest pulado}"
    else erro "falta $orig"
    fi
  fi
}

# confere <destino> | <mensagem>: so avisa se o arquivo (aceita * e ?) nao
# esta no disco. Para o que vai a mao (imagens grandes, dados do usuario).
faz_confere() {
  t=$(para_mtools "$1") || { erro "destino fora do D: $1"; return; }
  if [ -d "$DISCO" ]; then
    d=${t%/*}; d=${d#::}
    find "$DISCO$d" -maxdepth 1 -iname "${t##*/}" 2>/dev/null | grep -q .
  else
    mdir -i "$IMG" "$t" </dev/null >/dev/null 2>&1
  fi || aviso "${2:-falta $1}"
}

# executa <script> [args]: sh <script> <disco> [args], da raiz do repositorio.
# Para o que nao cabe numa lista (o Firefox e o QEMU tem scripts proprios).
faz_executa() {
  s=$1; shift
  [ -f "$s" ] || { erro "falta o script $s"; return; }
  if [ $SECO = 1 ]; then echo "  sh $s $DISCO $*"; return; fi
  sh "$s" "$DISCO" "$@" </dev/null || erro "$s falhou"
}

processa() {
  lst=$1
  COMP=$(basename "$lst" .lst)
  COPIADOS=0; PRESERVADOS=0; n=0
  while IFS= read -r linha <&3 || [ -n "$linha" ]; do
    n=$((n + 1))
    ONDE="$COMP.lst:$n"
    linha=${linha%"$CR"}
    msg=
    case "$linha" in *'|'*) msg=${linha#*|}; msg=${msg#"${msg%%[! ]*}"}; linha=${linha%%|*} ;; esac
    set -f; set -- $linha; set +f
    [ $# -gt 0 ] || continue
    case "$1" in
      \#*) ;;
      copia)
        if [ $# -lt 3 ]; then erro "uso: copia <origem> <destino> [opcoes]"
        else o=$2; d=$3; shift 3; faz_copia "$o" "$d" "$msg" "$@"
        fi ;;
      pasta)
        if [ $# -ne 2 ]; then erro "uso: pasta <destino>"
        else t=$(para_mtools "$2") && garante_pasta "${t%/}" || erro "pasta $2"
        fi ;;
      confere)
        if [ $# -ne 2 ]; then erro "uso: confere <destino> | <mensagem>"
        else faz_confere "$2" "$msg"
        fi ;;
      executa)
        if [ $# -lt 2 ]; then erro "uso: executa <script> [args]"
        else shift; faz_executa "$@"
        fi ;;
      *) erro "comando desconhecido: $1" ;;
    esac
  done 3<"$lst"
  r="$COPIADOS arquivo(s)"
  [ $PRESERVADOS = 0 ] || r="$r, $PRESERVADOS do usuario mantido(s)"
  [ $SECO = 0 ] || r="$r (simulado)"
  echo "install: $COMP: $r"
}

if [ $# -eq 0 ]; then
  set -- "$AQUI"/componentes/*.lst
  [ -f "$1" ] || { echo "install: nenhuma lista em $AQUI/componentes"; exit 1; }
fi
for c in "$@"; do
  case "$c" in
    */*|*.lst) lst=$c ;;
    *) lst="$AQUI/componentes/$c.lst" ;;
  esac
  [ -f "$lst" ] || { echo "install: componente $c nao existe ($lst)"; ERROS=$((ERROS + 1)); continue; }
  processa "$lst"
done
[ $ERROS = 0 ] || { echo "install: $ERROS erro(s)"; exit 1; }
exit 0
