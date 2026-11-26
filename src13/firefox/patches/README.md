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
| `0009-firefox-sodos-janela.patch` | `HeadlessWidget` ligado a janela do WIN (`widget/sodos/SodosWindow.cpp`) e `DocumentLoadListener` sem troca de processo quando nao ha e10s |
| `0010-nspr-sodos-poll.patch` | NSPR usa `poll()` do port no SODOS (`_PR_POLL_AVAILABLE`); a emulacao com `select()` nao via os fds de pipes e soquetes (>= 2000) e a thread de soquetes parava |
| `0011-firefox-sodos-freebl-static.patch` | freebl estatico: sem `FREEBL_NO_WEAK` no SODOS (o stubs.c usa os simbolos fracos do link, sem dlopen de libnspr4/libnssutil) e `STUB_SAFE_CALL5` no ramo fraco; sem isso `FREEBL_GetVector` devolvia NULL e o softoken dava `-8023` no `BL_Init` |
| `0012-firefox-sodos-roda.patch` | roda do mouse: `HeadlessWidget.h` usa no SODOS o modo de delta do GTK (linhas, 3 por passo); sem isso o `deltaMode` ficava -1 e o `EventStateManager` abortava (`MOZ_CRASH`) ao rolar |
| `0013-firefox-sodos-audio-nulo.patch` | backend `sodos` no cubeb (`USE_SODOS`, `cubeb_sodos.c` copiado de `sodos/cubeb` por `build-browser64.sh`): saida de audio nula que consome as amostras no ritmo do relogio, para o `<audio>`/`<video>` andar sem placa de som |
| `0014-firefox-sodos-jit-reserva.patch` | reserva de codigo do JIT de 256 MB no SODOS (`MaxCodeBytesPerProcess`); sem memoria virtual o mmap da reserva de 2044 MB virava RAM e o GC ficava sem chunks (`Failed to allocate new chunk during GC`) |
| `0017-firefox-sodos-primeira-aba.patch` | primeira aba da janela: sem e10s o `tabbrowser.js` cria o browser inicial no processo principal (`NOT_REMOTE`); antes, com a pagina inicial ainda em promessa, ele virava `remote="true"` sem processo de conteudo (`docShell` nulo) e nem a pagina inicial nem o Enter da barra carregavam. So JavaScript: no D: vale editar `D:\FIREFOX\browser\chrome\browser\content\browser\tabbrowser\tabbrowser.js` e criar `D:\FIREFOX\browser\.purgecaches` |
| `0016-firefox-sodos-ffmpeg-video.patch` | video: o `FFVPXRuntimeLinker` usa um caminho fixo no SODOS (sem `dladdr`, o ffvpx nunca ligava); com `sodos/scripts/build-ffmpeg.sh` o FFmpeg completo (H.264/HEVC/AAC/VP9) entra no FIREFOX.EXE no lugar do ffvpx e o `dlopen` do port expoe as funcoes dele |
| `0018-firefox-sodos-uptime.patch` | `NowIncludingSuspendMs` do mozglue usa `CLOCK_MONOTONIC` no SODOS (sem `CLOCK_BOOTTIME` e sem suspensao); antes devolvia `Nothing()`, o `ProcessUptimeMs()` ficava vazio e o `AudioSessionManager` abortava quando uma midia ficava audivel (so aparece com o som funcionando: `media.cubeb.sandbox=false` no `sodos.js`) |
| `mesa-0001-sodos.patch` | Mesa 25.0.7 para `host_machine.system() == 'sodos'` (KMS/DRM e `_GNU_SOURCE` como no Linux), sem `pthread_barrier`, futex Linux, inotify e `pthread_mutex_timedlock` (caem nos fallbacks do proprio Mesa) e sem o `static_assert` de `off_t` de 64 bits; aplica em `~/ffsrc/mesa-src` (`sodos/scripts/build-mesa.sh`), nao na arvore do Firefox |
| `llvm-0001-sodos.patch` | LLVM 18.1.8: `EX_IOERR` sem `sysexits.h` no SODOS; aplica em `~/ffsrc/llvm18/llvm` (`build-mesa.sh`) |
| `rust-std-sodos.patch` | `std` do Rust para `target_os = "sodos"`; aplica no `rust-src` do nightly (`sodos/scripts/patch-rust-std.sh`), nao na arvore do Firefox |

Os marcadores SODOS_MARK em js/src/shell/js.cpp (bytes na serial COM1)
sao temporarios de depuracao e **nao** fazem parte de nenhum patch.