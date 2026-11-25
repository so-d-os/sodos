# LIMPA — reinício leve, sem reiniciar o PC

`LIMPA` fecha todos os programas e roda o fim do boot de novo. A memória dos programas volta ao sistema, e o que fica aberto é só o que o boot abre: o AUTOEXEC.BAT, os serviços que ele liga, o WIN e o que o WIN abre sozinho (AIPANEL).

Fonte: `shell/cmd_limpa.c`, com os ganchos em `kernel/vt.c` (`vt_limpa_finish`), `video/visual.c` (`killq_service`) e `kernel/kernel.c` (`kernel_boot_programs`).

## Uso

```
LIMPA        fecha tudo e roda o boot de novo
LIMPA /T     só mostra o que fecharia (não mexe em nada)
```

Funciona no prompt de qualquer tela, num Command do WIN e pela API (`sh /d/WSL/SODOS limpa`). A resposta da API chega antes de o WIN fechar.

## O que fecha e o que fica

| Fecha | Fica |
|---|---|
| Programas do WIN, inclusive os que ele abriu sozinho (diferente do `KILLALL`) | Drivers, discos montados, a rede (o `NET WIFI START` do AUTOEXEC não religa a placa) |
| O WIN (abre de novo pelo `WIN` do AUTOEXEC) | Threads do kernel (temperatura, CONSOLE.LOG, RIT) |
| Programas das telas do DOS (Ctrl+F1..F12): PONTE, BAT.EXE, prompts | O log do Ctrl+F12 (o anel em memória) |
| Serviços: DISKSYNC, WSL (a VM desliga) | O cache de pastas do FAT32.DLL |
| WINE.DLL (sai quando não sobra EXE do Windows) | |
| Variáveis de ambiente criadas depois do boot (`SET` no prompt, `WINE=1`, `FAT32DLL`...) | |

Depois disso a tela 1 recomeça como no boot: as variáveis voltam ao que eram antes do AUTOEXEC.BAT, o AUTOEXEC.BAT roda (e abre o WIN), depois o HTTPD.EXE e o prompt. A tela 1 mostra a memória livre antes e depois.

## Como funciona

1. Com o WIN aberto, o `LIMPA` só marca o pedido. Se o WIN está ao fundo, ele vem para a frente. Na volta seguinte do laço do WIN, o `killq_service` encerra todas as tarefas com `mod_kill`, sem esperar que fechem sozinhas, e manda o WIN sair, como no menu Sair.
2. O `vt_win_entry` chama o `vt_select` para voltar à tela de antes. Com o pedido de LIMPA, o `vt_select` chama o `vt_limpa_finish`. Sem o WIN aberto, a tela que pediu chama o `vt_limpa_finish` direto.
3. O `vt_limpa_finish` cria a pilha nova da tela 1. Se não houver memória, nada muda. Depois larga todas as telas como estão e passa para a tela 1 nova, sem voltar. Uma tela ao fundo sempre está parada num ponto de troca (tecla, saída, `k_yield`), nunca no meio de uma operação de disco: o `sched_yield` não troca de tarefa com a trava do FS presa.
4. Já na pilha nova, `limpa_boot_entry` encerra os programas das telas largadas (`vt_limpa_after`), solta as pilhas delas e o WINE.DLL, zera a marca dos programas de início (`mod_mark_boot_reset`), volta as variáveis e roda `kernel_boot_programs(1)`.

## Limites

- Este Claude Code roda no WIN, então o `LIMPA` também o fecha.
- Kernel trocado pelo TROCA: as variáveis do boot só são guardadas no boot. Com um kernel que chegou a quente, o LIMPA não volta as variáveis (os `SET` feitos depois do boot continuam) até o próximo boot de verdade.
- Memória que o kernel reservou para si (caches, drivers) não volta: o LIMPA solta a memória dos programas.
