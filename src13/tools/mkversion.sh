#!/bin/sh
# mkversion.sh - gera kernel/version.h a cada build do kernel.
# O numero de build sobe em kernel/BUILDNO; junto vao a data/hora do build
# e o commit (com '+' se havia mudancas nao commitadas). O VER, o banner antes
# do prompt, o Gerenciador e o \LOG\BOOT.LOG mostram esta string.
cd "$(dirname "$0")/.." || exit 1
n=$(cat kernel/BUILDNO 2>/dev/null || echo 0)
n=$((n + 1))
echo "$n" > kernel/BUILDNO
git_rev=$(git rev-parse --short HEAD 2>/dev/null || echo semgit)
if ! git diff --quiet HEAD -- . 2>/dev/null; then git_rev="${git_rev}+"; fi
when=$(date '+%Y-%m-%d %H:%M')
cat > kernel/version.h <<EOH
/* Gerado por tools/mkversion.sh - nao editar. */
#ifndef SODOS_VERSION_H
#define SODOS_VERSION_H
#define SODOS_VER_BUILD $n
#define SODOS_VER_DATE "$when"
#define SODOS_VER_GIT "$git_rev"
#define SODOS_VERSION "SODOS 1.0 build $n ($when, $git_rev)"
#endif
EOH
echo "versao: SODOS 1.0 build $n ($when, $git_rev)"
