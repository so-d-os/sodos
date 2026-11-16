# 07 — WebGL em GL de software (Mesa llvmpipe dentro do FIREFOX.EXE)

## Problema

Sem WebGL o Firefox do SODOS dizia na serial `Failed to create WebGL context`
(`FEATURE_FAILURE_EGL_LOAD_3`, `FEATURE_FAILURE_WEBGL_EXHAUSTED_DRIVERS`). Apps
Flutter web (ex.: `sistema.c8x.app`) caem então no CanvasKit em software:
cada quadro é rasterizado em wasm na thread principal (CPU0). Com as
k_threads (doc 06) a tecla no campo CPF do C8X levava ~0,9 s (1,3–2,3 s com a
janela maximizada) e o caret piscando deixava a CPU0 em ~93% em código JIT,
com as APs paradas.

## Qual caminho de GL o Firefox 153 usa no SODOS

- O toolkit é `sodos` (HeadlessWidget + `widget/sodos/SodosWindow.cpp`). Em
  `gfx/gl/moz.build` só windows/cocoa/uikit/gtk/android escolhem provedor; o
  resto fica com o padrão de `GLContextProvider.h`, que é
  **`GLContextProviderEGL`** — já compilado no libxul.
- `GLLibraryEGL::DoEnsureInitialized` faz `PR_LoadLibrary("libEGL.so")` e
  `("libGLESv2.so")` e procura cada função por nome (`dlsym`), e as funções GL
  também via `eglGetProcAddress`. Era aí que falhava (`EGL_LOAD_3`): o
  `dlopen` do port (`sodos/port/sodos_extra.c`) só conhecia softoken/freebl.
- O WebGL cria o contexto com `GLContextProviderEGL::CreateHeadless` →
  `CreateDisplayLocked`. Fora do GTK, com `FORBID_HARDWARE` o display de
  software do GTK não existe e nada é tentado; sem a flag vai para
  `eglGetDisplay(EGL_DEFAULT_DISPLAY)`. `webgl.force-enabled=true` tira a
  flag (sem GPU o gfx marcaria tudo como "software").

Ou seja: **basta uma libEGL + libGLESv2 que responda por nome**. Não é preciso
mexer no Gecko.

## Escolha: Mesa 25.0 (EGL surfaceless + llvmpipe), estático

| Opção | Por que não / por que sim |
|---|---|
| softpipe (Mesa sem LLVM) | Fácil (primeiro protótipo, funcionou: "WebGL 2.0"), mas rasteriza numa thread só e é lento: no C8X a página travava em "script lento" no `canvaskit.wasm`. |
| SwiftShader / ANGLE | O GLES do SwiftShader saiu do upstream (só Vulkan + ANGLE); mais código e mais camadas que o Mesa. |
| **llvmpipe** | Shaders compilados pelo LLVM para x86-64 com SSE/AVX e rasterização em blocos por várias threads (k_threads nas APs). Escolhido. |

O Mesa 25.0 é o primeiro em que o libEGL liga o libgallium direto (sem
`dlopen` do `swrast_dri.so`), então tudo cabe num executável estático. A
plataforma EGL é só `surfaceless` (sem X11/Wayland/GBM): tenta os
`/dev/dri/renderD*` (não há; o libdrm devolve erro) e cai no swrast.

### Como entra no FIREFOX.EXE

`sodos/scripts/build-mesa.sh` (fontes e builds em `~/ffsrc`, fora do git):

1. baixa Mesa 25.0.7, libdrm 2.4.124 e LLVM 18.1.8, aplica
   `patches/mesa-0001-sodos.patch` e `patches/llvm-0001-sodos.patch`;
2. LLVM (só X86, estático, sem RTTI/EH, alvo "linux": o JIT gera ELF na
   memória) com CMake + `x86_64-sodos-g++64` (~20 min com 16 CPUs);
3. libdrm estático (só para o EGL do Mesa compilar; em execução as chamadas
   falham);
4. Mesa com meson (`host_machine.system = 'sodos'`, cross file gerado),
   `-Dplatforms= -Degl=enabled -Dgles2=enabled -Dgallium-drivers=llvmpipe`.
   O `llvm-config` é `sodos/mesa/llvm-config-sodos.py`, que responde pelo build
   cruzado (o de verdade é um EXE do SODOS);
5. junta os objetos de libEGL + libGLESv2 + libgallium (+ LLVM) com `ld -r`
   em **um objeto só** (`~/ffsrc/mesa-build/mesa_sodos.o`, ~134 MB) e
   localiza todos os símbolos fortes; só `eglGetProcAddress` fica global, com
   o nome `sodos_mesa_eglGetProcAddress`. Símbolos fracos/COMDAT (templates da
   libc++) continuam globais: localizados, o ld descartava o grupo COMDAT do
   Mesa (o libxul tem o mesmo) e sobravam referências para a seção descartada.

`link-firefox64.sh` liga esse objeto se ele existir (`MESA_OBJ=` para outro).
No port, `dlopen("libEGL…"/"libGLESv2…")` devolve um handle `DL_MESA` e
`dlsym(DL_MESA, nome)` chama o `eglGetProcAddress` do Mesa, que acha as
funções EGL (inclusive as do núcleo) e as GL. Sem o Mesa ligado, o símbolo
fraco é nulo e tudo fica como antes (WebGL desligado).

O FIREFOX.EXE cresce de ~215 MB para ~285 MB.

### O que o port ganhou (`sodos/port/sodos_mesa.c`, `sodos/mesa/include`)

- `sodos_mesa_compat.h`, incluído em toda unidade do Mesa/LLVM: protótipos
  que a libc do SODOS não tem, `PRIiMAX`, `nlink_t`, `_SC_ARG_MAX`, `EX_IOERR`;
  `linux/types.h`, `asm/ioctl.h`, `sys/sysmacros.h`, `sys/random.h`,
  `regex.h` (nada casa), `sysexits.h`.
- `sodos_mesa.c`: `asprintf`, `strchrnul`, `open_memstream`/`popen`/`mknod`
  (falham), `sync`, `openlog`, `pthread_getaffinity_np`, `ppoll`, `wait4`,
  `strsignal`, `getsid`, `feclearexcept`/`fetestexcept` (MXCSR) e os
  **`operator new(size_t, nothrow_t)`** (4 variantes). Sem estes o primeiro
  `new(std::nothrow)` do LLVM abortava: a libc++ sem exceções exige que quem
  troca o `operator new` (o mozalloc) troque também o nothrow.
- `toolchain/sodos-gcc.py` responde `--print-search-dirs` (o meson pergunta ao
  procurar o LLVM estático).

As threads do Mesa (rasterizadores do llvmpipe, filas) são pthreads do port →
k_threads nas APs (doc 06). O Mesa usa os fallbacks de mutex/cond em vez do
futex Linux e da barreira POSIX.

## Medições (QEMU/KVM, `-smp 4 -m 8192`, 2026-11-16)

Mesmo disco (`-snapshot`), FIREFOX.EXE com llvmpipe, kernel da branch das
k_threads. Latência = tecla (QMP `send-key`) até o dígito aparecer no campo,
por `screendump` do QMP a cada ~25 ms (não custa nada ao guest). Detector:
mais de 4 colunas do campo mudaram em relação à imagem de antes da tecla (o
caret piscando muda 2–3).

| C8X, campo CPF | antes (CanvasKit em software, k_threads) | WebGL llvmpipe |
|---|---|---|
| janela 1600×820 | ~0,9 s | mediana **0,26 s** (0,24–0,78, n=11) |
| janela maximizada | 1,3–2,3 s | mediana **0,44 s** (0,28–0,82, n=11) |
| CPU em repouso (caret piscando) | CPU0 ~93% em JIT | CPU0 e APs ~90% ociosas |

- `data:` com `getContext('webgl2')`: **"WebGL 2.0 / llvmpipe, or similar"**.
- C8X: tela de login desenhada ~20 s depois de digitar a URL; nenhuma linha
  `Failed to create WebGL`/`FEATURE_FAILURE` na serial.
- Amostras de RIP durante a digitação mostram `lp_rast_triangle_*` e código JIT
  nas APs; a CPU0 quase não roda o Firefox — o que sobra nela é o laço ocioso
  do WIN lendo a porta 0x64 do 8042 (`drain_locked`, cada `inb` é uma saída
  de VM), não trabalho de renderização.
- A latência é bimodal (~0,3 s ou ~0,8 s): parece esperar o próximo quadro do
  caret (500 ms). Não investigado.

## Limites conhecidos / próximos passos

- **softpipe**: com ele o C8X parava em "script lento" no `canvaskit.wasm` com
  as CPUs ociosas (thread principal bloqueada esperando algo, não calculando),
  tanto com `webgl.out-of-process` ligado quanto desligado. Com llvmpipe não
  acontece; a causa não foi achada. O build-mesa.sh ainda aceita
  `MESA_DRIVER=softpipe`.
- `[GFX1-]: Couldn't sanitize GL_RENDERER "softpipe"` só sai com softpipe; o
  llvmpipe é reconhecido ("llvmpipe, or similar").
- Não testado: `SET FFTHREADS=0` (pthreads cooperativas — as threads do
  llvmpipe dependem de k_threads de verdade para usar as APs), o notebook
  (`make real`), e páginas WebGL pesadas.
- `LP_NUM_THREADS` não é definido: o llvmpipe usa o número de CPUs que o port
  informa (`sysconf`/`sched_getaffinity`).

## Teste

```
sh src13/firefox/sodos/scripts/build-mesa.sh          # ~25 min na primeira vez
sh src13/firefox/sodos/scripts/link-firefox64.sh      # liga ~/ffsrc/mesa-build/mesa_sodos.o
```

Copiar FIREFOX.EXE e `sodos/prefs/sodos.js` (`webgl.force-enabled`) para o
disco (`copy-firefox-disk.sh`). No Firefox: `data:text/html,<script>
document.write(document.createElement('canvas').getContext('webgl2')
.getParameter(0x1F01))</script>` mostra `llvmpipe, or similar`.
