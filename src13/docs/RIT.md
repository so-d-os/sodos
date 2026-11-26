# RIT - Raw Input Thread do SODOS

Inspirada na RIT do Windows (win32k). Objetivo: a entrada do usuario nunca
espera o resto do sistema.

## O que faz

| Parte | Onde | O que faz |
|---|---|---|
| Thread de tempo real | `video/visual.c` (`rit_main`), `kernel/kthread.c` (`kt_thread_create_rt`) | Presa a ultima CPU. Com 3+ CPUs essa CPU fica so para ela (`SET RITRES=0` divide). Nunca pega o BKL. |
| Acordar na hora | `kernel/ps2in.c` (`ps2in_kick`), `kernel/isr.c` vetor `0xF5`, `kernel/xhci.c` (`xhci_rit_irq`) | IRQ1/IRQ12 (PS/2) e MSI do xHCI acordam a RIT. Sem MSI: polling de 1 ms do anel do xHCI e do tablet UHCI. |
| Leitura | `rit_pump` | Mouse PS/2 (pacotes de 9 bits), tablet UHCI (QEMU), mouse USB (`xhci_rit_poll`). As teclas USB vao para a fila do PS/2, com repeticao. |
| Seta | `rc_*` em `video/visual.c` | A RIT move a seta na pagina visivel. Funciona com back buffer copiado pela CPU, com pagina dupla do Bochs e com o driver AMDGOP (o fundo vem do back buffer e a seta volta depois de cada present). |
| Fila de eventos | `rq`, `rit_take` | Posicao absoluta, botoes, roda e tick de chegada. O mundo junta movimentos seguidos num so evento. |
| Prioridade | `kernel/kthread.c` | O mundo primeiro, depois a RIT, depois as threads do programa em foco (`kt_boost_pid`, 100 ms a cada entrada). `kt_world_kick` tira o WIN do emprestimo ocioso da CPU0. |

A trava do driver xHCI (`xl_enter`/`xl_try`) garante um consumidor por vez
no anel de eventos. A RIT so tenta pegar a trava: se o driver esta ocupado
(pendrive, enumeracao), ela tenta de novo na proxima volta. Reset de
endpoint e portas ficam com o mundo (`xhci_mouse_maint`).

## Comandos e variaveis

- `RIT`: mostra a CPU, o modo (MSI, IRQ ou polling), os contadores e a
  latencia de chegada ate o WIN (mouse e teclado). `RIT Z` zera a latencia.
- `SET RIT=0`: desliga a RIT (o mundo le tudo, como antes).
- `SET RITCUR=0`: a RIT le a entrada, mas a seta fica com o mundo.
- `SET RITRES=0`: nao reserva a CPU da RIT (as threads tambem rodam nela).

## Medido no QEMU (KVM, 4 CPUs)

- Mouse e teclado (PS/2, tablet, USB por polling e por MSI): chegada ate o
  WIN em menos de 1 ms em quase todos os eventos, maximo de 1 a 3 ms.
- Mundo travado 46 s num `xcopy` pelo HTTPD: a seta acompanhou o mouse o
  tempo todo e os 41 eventos chegaram ao WIN quando ele voltou.
- Com MSI a RIT so acorda quando ha evento (246 voltas em vez de ~1000 por
  segundo).

## Seta sem piscar (build 76)

Com back buffer copiado pela CPU, cada quadro copiava a tela inteira (9 MB
em 1920x1200) por cima da seta e so no fim `rc_shown` a desenhava de novo.
Com um programa animando (o Claude, ~10 quadros/s com o mouse parado:
`RIT` mostrava `seta=` subindo sem `eventos=` mudar), a seta sumia por
uns milissegundos a cada quadro e o mouse piscava. O mesmo no
`wm_present_win` (tecla no Command): `cursor_restore` antes de pintar a
janela inteira.

Agora `rc_copy_rect` copia linha a linha e, nas linhas da seta, compoe a
seta sobre o fundo do back buffer antes de gravar: a tela nunca fica sem
ela, e o fundo guardado (`cursor_under`) sai do proprio back buffer. Vale
para o quadro inteiro (`gfx_copy_back`) e para a janela sozinha
(`wm_present_win`). Pagina dupla e driver de placa seguem como antes
(`rc_copy_ok`).

## Limites

- O programa so reage quando o laco do WIN o roda: se o mundo para, a seta
  anda mas a janela so responde quando ele volta.
- Com tela estendida ou cursor da placa (`gpu_hwcur`), a seta fica com o
  mundo, como antes.
