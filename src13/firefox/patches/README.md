# Patches sobre o upstream

A política é a mesma do port do NetSurf: **um patch por assunto**, numerado,
aplicável com `patch -p1` na raiz da árvore baixada em `vendor/`.

```
0001-firefox-sodos-target.patch   # alvo x86_64-sodos no configure (config.sub,
                                  # init.configure, constants.py)
0002-nspr-sodos-md.patch          # M1: configuração de alvo e MD do NSPR
0003-mozglue-sodos.patch          # M2: TimeStamp/Logging/Malloc
0004-widget-sodos.patch           # M5: nsWindow sobre k_gui_*
...
```

Regras:

- O patch não pode tocar em `vendor/` já aplicado; gere a partir de
  `vendor/<upstream>` com `diff -ruN`.
- Todo patch tem um cabeçalho com fase, arquivo afetado e teste que cobre.
- Depois de aplicar, `docs/03-fases.md` e o README do port são atualizados.
- Nada de patch “por acidente” de build (flags, caminhos): isso vai no
  `mozconfig`/Makefile.

## Patches existentes (estado atual)

| Arquivo | Assunto |
|---|---|
| `001-firefox-sodos-target.patch | alvo x86_64-sodos no configure |
| `002-sodos-rust-bringup.patch | bring-up do Rust (core/alloc/std) |
| `003-firefox-sodos-build.patch | ajustes de build do SpiderMonkey |
| `004-js-shell-sodos-file-io.patch | ler .js sem stat (FILEs da libc sao em memoria) |
| `rust-std-sodos.patch` | `std` do Rust para `target_os = "sodos"`; aplica no `rust-src` do nightly (`sodos/scripts/patch-rust-std.sh`), nao na arvore do Firefox |

Os marcadores SODOS_MARK em js/src/shell/js.cpp (bytes na serial COM1)
sao temporarios de depuracao e **nao** fazem parte de nenhum patch.