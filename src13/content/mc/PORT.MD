# Port hibrido: GNU Midnight Commander no SODOS

## Origem do upstream

| Campo | Valor |
|---|---|
| Destino no host | `src13/vendor/mc/` |
| Repositorio | https://github.com/MidnightCommander/mc |
| Tag / versao | **4.8.33** |
| Commit | `e7e21869da5a2651ebccce5274e47fb3e607eba6` |
| Tamanho aproximado | ~19 MiB (clone raso) |
| Licenca | GPL-3.0-or-later |

O diretorio `vendor/mc/` **nao** entra no git (ver `.gitignore`). So as notas em `content/mc/` sao versionadas.

## Fetch (host Linux)

A partir de `src13/`:

```bash
./content/mc/FETCH.sh
# ou manualmente:
mkdir -p vendor
git clone --depth 1 --branch 4.8.33 \
  https://github.com/MidnightCommander/mc.git vendor/mc
```

## Configure minimo (meta)

Quando o sysroot SODOS existir:

```text
./configure \
  --disable-vfs \
  --without-subshell \
  --without-internal-edit \
  --without-diff-viewer \
  --disable-nls \
  --without-x \
  --without-gpm-mouse \
  --disable-background \
  --with-screen=ncurses \
  --host=x86_64-sodos-x32 \
  --prefix=/
```

Host real do toolchain: `gcc -mx32` (long mode + ponteiros 32 bits).
Ainda faltam ncurses shim e glib; posix-lite e gcc->SOX ja existem.

## Gaps vs SODOS hoje

| Precisa | SODOS agora |
|---|---|
| libc POSIX (open, dirent, unistd, ...) | `LIBC.DLL` + `k_*` em `so.h`; sem dirent/termios/fork |
| glib >= 2.32 | ausente |
| ncurses (ou S-Lang) | ausente (VGA texto / GUI.DLL) |
| gcc C completo | `cc` interno sem float; apps grandes: `gcc -mx32` + `LIBCX.DLL` |
| FAT / nomes | 8.3; arvore MC so no host (`vendor/mc`) |

## Inventario de dependencias (tag 4.8.33, arvore completa)

Inspecao em `vendor/mc/src` e `vendor/mc/lib` (contagens = arquivos que citam o simbolo). Com `--disable-vfs --without-subshell --disable-background` boa parte de rede/`fork` some do link, mas o nucleo do file manager ainda precisa do bloco POSIX abaixo.

### Bibliotecas de build (INSTALL + m4)

| Lib | Minimo | Uso no MC |
|---|---|---|
| glib-2.0 | >= 2.32 | obrigatorio (`mc-glib.m4`) |
| gmodule-2.0 | >= 2.32 | opcional (X11 / plugins) |
| ncurses | — | `--with-screen=ncurses` (tty-ncurses.c) |
| gettext | >= 0.18 | desligavel com `--disable-nls` |
| pcre / pcre2 | — | alternativa a glib-regexp; evitavel se glib regex bastar |
| libssh2, aspell, ext2fs, gpm | — | fora do build minimo |

### Headers de sistema mais citados (amostra)

`dirent.h`, `fcntl.h`, `unistd.h`, `signal.h`, `termios.h`, `sys/stat.h`, `sys/wait.h`, `sys/ioctl.h`, `sys/select.h`, `pwd.h`, `grp.h`, `locale.h`, `langinfo.h`, `dlfcn.h`, `sys/socket.h`, `netdb.h`, `glib.h`, `ncurses.h` / `curses.h`.

### POSIX / libc — prioridade para posix-lite

| Simbolo | # arquivos | Prioridade no minimo |
|---|---:|---|
| `stat` | 82 | alta (listagem) |
| `read` / `write` / `open` / `close` | 51 / 38 / 42 / 37 | alta |
| `select` | 29 | media (input/tty; pode virar poll do kernel) |
| `signal` / `kill` | 20 / 12 | baixa no minimo sem subshell |
| `lstat` / `unlink` / `mkdir` / `rename` | 14–15 | alta |
| `chdir` / `opendir` / `readdir` / `closedir` | 9–10 | alta (mapear para `k_flist` / FAT) |
| `pipe` / `waitpid` / `fork` | 16 / 6 / 4 | baixa com `--without-subshell --disable-background` |
| `tcgetattr` / `tcsetattr` / `ioctl` | 3 / 3 / 8 | media (term shim) |
| `getpwnam` / `getgrnam` | 4 | baixa (UID/GID stub) |
| `socket` / `connect` | 3 | fora com `--disable-vfs` |

### glib — APIs mais usadas (guia do subset)

Ordem por ocorrencias aproximadas: `g_free`, `g_strdup`, `g_list_*`, `g_string_*`, `g_snprintf` / `g_strdup_printf`, `g_ptr_array_*`, `g_strconcat`, `g_new` / `g_new0` / `g_malloc`, `g_build_filename`, `g_get_monotonic_time`, UTF-8 (`g_utf8_*`), `g_slist_*`, `g_array_*`.

Isso define o **glib subset** da fase 4 (ou portar glib real via gcc->SOX).

### ncurses — superficie no `lib/tty`

Arquivos: `tty-ncurses.c`, `color-ncurses.c`, `key.c`, `win.c`, …

Simbolos observados: `initscr`, `endwin`, `keypad`, `noecho`, `cbreak`, `raw`, `has_colors`, `start_color`, `init_pair`, `LINES`, `COLS`, `ACS_*` (cantos/linhas). O restante do UI passa pela camada `tty.h` do proprio MC (bom ponto para o term shim).

## Ordem de implementacao

1. **posix-lite** — FEITO: `POSIX.DLL` + `k_pwd`/`k_chdir`/`k_dirlist`.
2. **gcc -> SOX** — FEITO: `gcc -mx32` + linker ELF/RELA + `LIBCX.DLL` + `XHELLO.EXE`.
   - ABI SysV (regs); nao misturar com `LIBC.DLL` do `cc` (args na pilha).
   - Flags: ver `XGFLAGS` no `c_compiler/Makefile`.
3. **term shim** — FEITO (base): `TERM.DLL` + `curses.h`; prova `TTERM.EXE`.
   - Falta: attrs VGA reais, keypad map completo, newwin/scroll.
4. **glib** — FEITO (subset): `GLIB.DLL` + `glib.h`; prova `TGLIB.EXE`.
5. **MC.EXE** — FEITO (hibrido): `examples/mc.c` → `out/MC.EXE` (2 paineis).
   - `POSIXX.DLL` = posix-lite em SysV (`gcc -mx32`).
   - Upstream `vendor/mc` (5b): ainda nao linka; falta expandir glib/term/POSIX.

## Layout

```text
src13/content/mc/   notas 8.3 para o desktop (esta pasta)
src13/vendor/mc/    fontes upstream 4.8.33 (host; fora do git)
src13/libs/         posix-lite + term + glib (feitos); falta linkar o MC
```
