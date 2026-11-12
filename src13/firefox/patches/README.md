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
| `0005-firefox-sodos-browser.patch` | toolkit `cairo-sodos`, diretorio `widget/sodos` e NSPR da arvore no alvo |
| `0006-firefox-sodos-linux-abi.patch` | alvo SODOS com ABI e fontes do Linux no Gecko (`OS_ARCH`, `XP_LINUX`), backend `widget/sodos` ligado ao build, FreeType da arvore e inicializacao sem `dlopen` |
| `0007-firefox-sodos-libclang16.patch` | `static_assert` de `nsThreadUtils.h` dependente de `T` tambem no clang anterior ao 17 (libclang 16 do bindgen) |
| `0008-firefox-sodos-sem-omnijar.patch` | `gfxFT2FontList::FindFontsInOmnijar` nao desreferencia o leitor nulo quando nao ha `omni.ja`; o `--screenshot` do HeadlessShell usa um browser local quando nao ha e10s |
| `rust-std-sodos.patch` | `std` do Rust para `target_os = "sodos"`; aplica no `rust-src` do nightly (`sodos/scripts/patch-rust-std.sh`), nao na arvore do Firefox |

Os marcadores SODOS_MARK em js/src/shell/js.cpp (bytes na serial COM1)
sao temporarios de depuracao e **nao** fazem parte de nenhum patch.