# 01 — Avaliação: o que o Firefox exige e o que o SODOS tem

Data: 2026-11-03. Base: código em `src13/` e build atual (`kernel.bin` com
369.164 bytes, `os.img` com 372.736 bytes).

Este documento separa o que é **possível começar agora** do que é **pré-
requisito de kernel**. A conclusão curta: o SODOS tem uma base boa de
arquivos, rede e janela, mas o Gecko exige um modelo de processo/thread e de
memória que o kernel ainda não tem. O port começa pela camada de plataforma
(NSPR) porque é ela que define esse contrato.

## 1. O que o Firefox exige

| Requisito | Detalhe | Onde entra no SODOS |
|---|---|---|
| Compilador C++17 | Gecko é C++ moderno (templates, `constexpr`, `std::`), clang ≥ 17 é o oficial; gcc também compila | não existe C++ no SO; cross-build com gcc/clang no host |
| Rust | componentes `style`, `webrender`, `xpcom` usam Rust; versão mínima acompanha o ESR | `rustc` para alvo `sodos` (Tier 3 custom) |
| Python 3 + Node | `mach`, `configure`, geração de código | fora do SO (host) |
| NSPR | threads, locks, tempo, arquivos, rede, DNS, processos | **M1** deste port |
| NSS | TLS, certificados, criptografia | hoje há o TLS do `c_compiler/tls` (1.2/1.3), não é NSS |
| ICU | Unicode, collation, formatação | não existe; pode ser `--without-intl-api` no começo |
| jemalloc | alocador do Gecko | desligar (`--disable-jemalloc`) até haver `mmap` de verdade |
| zlib/libpng/libjpeg/sqlite | imagem, cache, histórico | zlib/png/jpeg existem no `c_compiler` e no `img`; sqlite não |
| cairo/pixman/harfbuzz/freetype | desenho e texto | o SODOS tem rasterizador TTF e `k_gui_*`, não cairo |
| GTK/X11/Wayland | toolkit do browser | não existe; o backend tem que ser `widget/sodos` sobre `k_gui_*` |
| Threads preemptivos | thread pool, GC, rede assíncrona | **não existe** (ver §3) |
| Memória virtual | páginas sob demanda, `mprotect`, W^X | **não existe** (heap único, sem swap) |
| Muitos sockets + `poll` | HTTP/2, DNS, QUIC | o kernel tem **uma** conexão TCP por vez |
| Processos | e10s/Fission: conteúdo em processo separado | `k_exec` roda na mesma memória; não há fork/ASLR |
| Sistema de arquivos | `omni.ja`, perfil, cache, downloads | FAT16 com leitura/gravação do arquivo inteiro |

Números de referência (build Linux x86_64, sem debug): `libxul.so` fica na casa
de 100 MB; um build completo do Firefox usa 8–16 GB de RAM e 20–60 min em 8
núcleos. Isso não cabe no modelo atual de carregar a imagem inteira na memória
a cada execução.

## 2. O que o SODOS já oferece

| Peça | Estado | Evidência |
|---|---|---|
| Kernel 64 bits, ring 0, identidade | estável | `kernel/kernel_entry.asm`, `kernel/linker.ld` |
| Alocador de heap | `k_malloc/k_free/k_realloc`, heap do usuário até o topo da RAM | `kernel/mm.c`, `kernel/kexports.c` |
| Arquivos FAT16 | ler, gravar, listar, apagar, pastas; leitura/gravação do arquivo inteiro | `fs/`, `libs/posix-lite/posix.c` |
| POSIX parcial | `open/read/write/lseek/stat/opendir/...` em `POSIX.DLL`/`POSIXX.DLL` | `libs/posix-lite/` |
| Rede IPv4 | ARP, ICMP, UDP/DNS, TCP com janela de 256 KB, SACK, OOO | `net/net.c` |
| TLS 1.3/1.2 | cliente com verificação de certificado | `c_compiler/tls/` |
| Janela/eventos | `k_gui_open/pixels/present/event`, teclado, mouse, roda | `kernel/kexports.c`, `gui/` |
| Tarefas cooperativas | `sched_yield`, até 8 jobs, sem preempção | `kernel/sched.c` |
| Toolchain x32 | gcc `-mx32` → ELF → `link` → SOX; DLLs com imports | `c_compiler/Makefile`, `vendor/netsurf/sodos` |
| Port de referência | NetSurf 3.11 rodando com LIBCX/POSIXX e shims | `vendor/netsurf/sodos/README.md` |

O port do NetSurf é o precedente mais importante: ele mostra que dá para
compilar código Unix grande com `gcc -mx32` e ligar num SOX, e que a camada
que falta (POSIX, headers, `scandir`, `mmap`, `iconv`) pode viver no próprio
port. O Firefox é o mesmo caminho, com um contrato de plataforma (NSPR) muito
maior.

## 3. As lacunas, por gravidade

### P0 — bloqueiam qualquer build do Gecko

1. **C++ runtime.** Não há libstdc++/libc++ para x32+SODOS, nem exceções,
   RTTI, `__cxa_*`, `.init_array`. O `link` do SODOS não roda construtores
   estáticos (o port do NetSurf registra as superfícies na mão por isso).
2. **Threads preemptivos.** `sched.c` só troca quando o programa chama
   `sched_yield`/`k_ms`/`k_gui_event`; não há pilha de kernel por thread nem
   IRQ de timer salvando contexto. A `libsodos` implementa pthreads
   cooperativos (M0), o que serve para NSPR, mas não para o thread pool e o
   GC do Gecko em produção.
3. **Memória virtual.** Sem `mmap` de arquivo, sem páginas sob demanda, sem
   `mprotect` real. O JIT do SpiderMonkey precisa de W^X; o `omni.ja` e as
   fontes são mapeados.
4. **NSS/ICU.** Sem eles o build do browser não fecha; o TLS atual é um
   cliente próprio, não a API do NSS.

### P1 — bloqueiam o uso (mas não o build)

5. **Uma conexão TCP.** `net/net.c` tem um único TCB (`tcp_state`, `tcp_*`).
   HTTP/2, DNS concorrente e e10s precisam de uma tabela de conexões e
   `poll` por socket.
6. **Sem `poll` de verdade.** Adicionamos `k_net_pending()` (bytes no buffer
   RX); não há espera por múltiplos fds, `POLLOUT` real nem timeout no kernel.
7. **Sem processos.** `k_exec` chama a imagem na mesma memória; não há fork,
   ASLR, nem isolamento. Fission exige processos.
8. **Sem fsync/append.** O log e o cache do Firefox reescrevem arquivos
   inteiros; um append real no FAT16 é M1/M2.

### P2 — desempenho e fidelidade

9. **Imagem SOX carregada inteira em ring 0.** Um `libxul` de 100 MB seria
   copiado do ATA a cada abertura; sem páginas de arquivo isso é proibitivo.
10. **`k_ms` de 32 bits** (wrap de ~49 dias). A `libsodos` já estende para
    64 bits.
11. **Um core, sem FPU context switch.** SSE é caller-saved na ABI, então a
    troca cooperativa funciona; threads preemptivos vão precisar salvar
    MXCSR/x87.
12. **Sem ASLR/sandbox.** O Gecko moderno assume sandbox; o port terá que
    compilar com `--disable-sandbox` e rodar tudo confiável.

## 4. Decisões que o projeto precisa tomar

| Decisão | Opções | Recomendação para o M1 |
|---|---|---|
| ABI | continuar x32 (ILP32) ou migrar para 64 bits | continuar x32 até o NSPR/SpiderMonkey; o Gecko suporta 32 bits e o SOX é u32 |
| Threads | cooperativos (usuário) ou preemptivos (kernel) | cooperativos no M0/M1; preempção no kernel em paralelo (M1b) |
| TLS | NSS ou o cliente atual | NSS no M4; o cliente atual serve para baixar fontes/testes |
| Escopo | Firefox completo ou primeiro o SpiderMonkey | NSPR → SpiderMonkey → Gecko; o shell do JS é o primeiro binário “de verdade” |
| Versão | ESR (estável) ou nightly | ESR; fixar a versão em `vendor/` e nos patches |

## 5. Critério de saída do M0 (cumprido)

- `SODOS.DLL` compila com gcc x32 e liga como DLL SOX.
- `FBOOT.EXE` passa 17 testes no `soxrun` (18 com o GET HTTP `net`).
- `k_yield`, `k_net_pending` e `k_exe_drive` existem no kernel e no
  `KERNEL.IMP`; o `soxrun` acompanha.
- O build do SO (`make` em `src13/`) continua verde.
- A documentação de arquitetura e fases está escrita.

O **M1** também está cumprido: o NSPR 4.35 compila (56 fontes) contra a
`libsodos` e o `PRTEST` passa 16/16 no `soxrun` e dentro do SODOS no QEMU
(threads, locks, condição, TLS, tempo, arquivos e `PR_snprintf`). Rede do NSPR
continua para o M4. O próximo passo verificável é o **M1b** (preempção no
kernel) e o **M2** (runtime C++/mozglue).
