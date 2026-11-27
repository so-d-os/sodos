#!/bin/sh
# mkversion.sh - gera kernel/version.h (VER, banner antes do prompt,
# Gerenciador, TROCA e \LOG\BOOT.LOG mostram esta string).
#
# O numero de build acompanha o codigo: e a quantidade de commits ate o HEAD
# (git rev-list --count HEAD). O mesmo commit da o mesmo numero em qualquer
# maquina, e cada commit novo sobe um. No clone raso do GIT.EXE do SODOS (so o
# ultimo commit) o git nao sabe essa conta: ela vem uma vez da API do GitHub
# para o commit do clone/pull e fica em .git/sodos-build ("commit numero").
#
# Compilado com mudancas nao commitadas (em src13), o build ganha +K e o
# commit um '+': "build 239+2 (..., ff02253+)" e o segundo kernel de teste em
# cima do commit 239. O K fica em kernel/BUILDLOC (fora do git) e volta a 0
# quando o codigo esta todo commitado. A versao do SO (ex.: 1.0) vem de
# kernel/VERSION.
#
#   sh tools/mkversion.sh            gera de novo (make version); sujo: K+1
#   sh tools/mkversion.sh --commit   so se falta o version.h ou ele e de outro
#                                    commit (o Makefile roda a cada make)
#   sh tools/mkversion.sh --kernel   kernel.elf sendo recompilado: sujo, K+1
#                                    (a nao ser que o version.h ja seja deste
#                                    build); limpo, volta a K=0
cd "$(dirname "$0")/.." || exit 1
modo=${1:-}
H=kernel/version.h
L=kernel/BUILDLOC

sha=$(git rev-parse HEAD 2>/dev/null)
[ -n "$sha" ] || sha=semgit
old_sha=$(sed -n 's/^#define SODOS_VER_COMMIT "\(.*\)".*/\1/p' "$H" 2>/dev/null)
old_k=$(sed -n 's/^#define SODOS_VER_LOCAL \([0-9]*\).*/\1/p' "$H" 2>/dev/null)

if [ "$modo" = --commit ] && [ -f "$H" ] && [ "$old_sha" = "$sha" ]; then exit 0; fi
# --kernel: o version.h deste commit e mais novo que o ultimo kernel.elf (veio
# do --commit deste mesmo make ou de um make version): ja e deste build
if [ "$modo" = --kernel ] && [ -f "$H" ] && [ "$old_sha" = "$sha" ] &&
  { [ ! -f kernel.elf ] || [ "$H" -nt kernel.elf ]; }; then
  exit 0
fi

# Sujo = mudanca de conteudo em src13. --ignore-cr-at-eol: o git do WSL num
# checkout do Windows ve CRLF em tudo; --diff-filter=t: o GIT.EXE grava os
# symlinks como arquivo comum (typechange), e isso nao e mudanca de codigo.
dirty=0
if [ "$sha" != semgit ] && ! git diff --quiet --ignore-cr-at-eol --diff-filter=t HEAD -- . 2>/dev/null; then
  dirty=1
fi

# --kernel com o codigo limpo e o version.h ja dizendo isso: nada a fazer
if [ "$modo" = --kernel ] && [ "$dirty" = 0 ] && [ "${old_k:-0}" = 0 ] && [ "$old_sha" = "$sha" ]; then
  exit 0
fi

# Contagem pela API do GitHub ($1 = commit), para o clone raso.
conta_github() {
  url=$(git config --get remote.origin.url 2>/dev/null)
  slug=$(printf '%s\n' "$url" | sed -n 's#^.*github\.com[:/]\([^/]*/[^/]*\)$#\1#p' | sed 's#\.git$##')
  [ -n "$slug" ] || return 1
  api="https://api.github.com/repos/$slug/commits?sha=$1&per_page=1"
  if command -v curl >/dev/null 2>&1; then
    h=$(curl -sSI -m 20 -H "Accept: application/vnd.github+json" "$api" 2>/dev/null)
  elif command -v wget >/dev/null 2>&1; then
    h=$(wget -S --spider -T 20 -q "$api" 2>&1)
  else
    return 1
  fi
  h=$(printf '%s\n' "$h" | tr -d '\r')
  printf '%s\n' "$h" | grep -q '^ *HTTP/[0-9.]* 200' || return 1
  # uma pagina por commit: a ultima pagina e a contagem (sem "link", so 1)
  p=$(printf '%s\n' "$h" | grep -i '^ *link:' | sed -n 's/.*[?&]page=\([0-9][0-9]*\)>; rel="last".*/\1/p')
  echo "${p:-1}"
}

# Clone raso: contagem do commit da borda (cache ou GitHub) + commits locais
# depois dele. Com mais de uma borda, fica a maior conta.
conta_raso() {
  gd=$(git rev-parse --absolute-git-dir 2>/dev/null) || return 1
  best=
  for b in $(cat "$gd/shallow" 2>/dev/null); do
    git merge-base --is-ancestor "$b" HEAD 2>/dev/null || continue
    c=$(sed -n "s/^$b \([0-9][0-9]*\)\$/\1/p" "$gd/sodos-build" 2>/dev/null | head -n 1)
    if [ -z "$c" ]; then
      c=$(conta_github "$b") || continue
      echo "$b $c" >> "$gd/sodos-build"
    fi
    t=$((c + $(git rev-list --count "$b..HEAD")))
    if [ -z "$best" ] || [ "$t" -gt "$best" ]; then best=$t; fi
  done
  [ -n "$best" ] && echo "$best"
}

n=
if [ "$sha" != semgit ]; then
  if [ "$(git rev-parse --is-shallow-repository 2>/dev/null)" = true ]; then
    n=$(conta_raso)
  else
    n=$(git rev-list --count HEAD 2>/dev/null)
  fi
fi
num=$n
if [ -z "$n" ]; then
  n=0
  num="?"
  echo "mkversion: sem a contagem de commits (sem git, ou clone raso sem rede para a API do GitHub): build ?" >&2
fi

k=0
if [ "$dirty" = 1 ]; then
  lsha=
  lk=
  [ -f "$L" ] && read -r lsha lk < "$L"
  case "$lk" in '' | *[!0-9]*) lk= ;; esac
  if [ "$lsha" = "$sha" ] && [ -n "$lk" ]; then k=$((lk + 1)); else k=1; fi
  num="$num+$k"
fi
printf '%s %s\n' "$sha" "$k" > "$L"

ver=$(tr -d ' \r\n' < kernel/VERSION 2>/dev/null)
[ -n "$ver" ] || ver=1.0
git_rev=$(git rev-parse --short HEAD 2>/dev/null || echo semgit)
[ "$dirty" = 1 ] && git_rev="${git_rev}+"
when=$(date '+%Y-%m-%d %H:%M')
cat > "$H" <<EOH
/* Gerado por tools/mkversion.sh - nao editar. */
#ifndef SODOS_VERSION_H
#define SODOS_VERSION_H
#define SODOS_VER_NUMBER "$ver"
#define SODOS_VER_BUILD $n
#define SODOS_VER_LOCAL $k
#define SODOS_VER_DATE "$when"
#define SODOS_VER_GIT "$git_rev"
#define SODOS_VER_COMMIT "$sha"
#define SODOS_VERSION "SODOS $ver build $num ($when, $git_rev)"
#endif
EOH
echo "versao: SODOS $ver build $num ($when, $git_rev)"
