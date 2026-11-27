# Você está rodando dentro do SODOS

Este Claude Code roda no SODOS (um sistema próprio, parecido com o DOS, com o desktop WIN) através do WINE. A ferramenta Bash não roda no SODOS: ela roda numa VM Linux (o WSL do SODOS). Na VM, as unidades do SODOS aparecem como `/d`, `/z`, `/u`... (`D:\X\Y` = `/d/X/Y`). As ferramentas Read, Write e Edit usam os caminhos do Windows (`D:\...`) direto.

Este arquivo vem do repositório do SODOS (`src13/docs/CLAUDE-SODOS.md`) e o `make send` o sobrescreve a cada instalação. Para mudar o texto, mude no repositório.

O que segue precisa do SODOS build 49 ou mais novo (o comando `ver` mostra o build) e do serviço do WSL ligado (`SET WSL=1`). Se `sh /d/WSL/SODOS ver` não responder, avise o usuário em vez de tentar outro caminho.

## Rodar comandos do SODOS a partir do Bash

O Linux não executa programas do SODOS. Para rodar uma linha do prompt do SODOS e ver a saída:

```sh
sh /d/WSL/SODOS <linha do prompt do SODOS>
```

O script manda a linha para a API do SODOS (10.0.3.2:8797) e devolve a saída, até 32 KB. Os caminhos `/d/x/y` viram `D:\x\y`; opções como `/w` não são alteradas. Escreva os caminhos como `/d/...`: no `sh`, um `D:\x` sem aspas perde a barra. `|`, `>` e `<` vão para o shell do SODOS. A linha tem no máximo 240 caracteres, e os pedidos são atendidos um por vez.

Cuidado: a linha roda dentro do laço do WIN, e o desktop fica parado até ela terminar. Use para comandos rápidos (`dir`, `type`, `copy`, `cc` de um arquivo, os comandos abaixo). Programas longos ou interativos devem ir por `command`.

## Comandar a tela, o teclado e o mouse

| Linha | O que faz |
|---|---|
| `command PROG.EXE args` | Abre um Command no WIN e roda a linha nele. Programas de texto (TOP, MC, AGENT2, EDIT) e programas longos (make) ganham tela própria. Volta na hora; para ver o resultado, tire um print. |
| `prtsc` | Print da tela à vista, pela mesma rotina da tecla PrtSc. Mostra o caminho do PNG em `D:\TEMP\printscreen-AAAAMMDD-HHMMSS.png`; abra com o Read. `prtsc /d/X.PNG` grava mais uma cópia ali. |
| `janelas` | Janelas do WIN: título, x, y, largura e altura. `*` marca a que tem o foco. A tela é 1920x1080 e o print tem as mesmas coordenadas. |
| `mouse X Y click` | Move e clica com o botão esquerdo. Também existem `direito`, `duplo`, `meio`, `desce`/`sobe` (arrastar) e `mouse pos`. Sem argumentos, `mouse` abre o teste interativo do mouse e trava: não use assim. |
| `tecla texto{ENTER}` | Digita na janela com foco, como o teclado de verdade, acentos incluídos. Teclas: `{ENTER} {TAB} {ESC} {BS} {DEL} {CIMA} {BAIXO} {ESQ} {DIR} {HOME} {END} {PGUP} {PGDN} {F1}..{F12} {PRTSC}`, combinações `{CTRL+C} {ALT+TAB} {SHIFT+TAB}`, repetição `{TAB 3}` e caracteres literais `{>} {<} {|} {{} {}}`. |

Um ciclo típico para testar um programa:

```sh
sh /d/WSL/SODOS command /d/DEVELOP/AGENT2/AGENT2.EXE
sleep 2; sh /d/WSL/SODOS prtsc          # ver a tela (Read no PNG mostrado)
sh /d/WSL/SODOS 'tecla oi{ENTER}'       # digitar no programa
sh /d/WSL/SODOS 'tecla {F10}'           # sair
```

## Trocar kernel ou WINE.DLL sem reiniciar

Num kernel com a troca a quente (`TROCA` sem argumentos responde), um kernel ou WINE.DLL novo não precisa de reboot:

| Linha | O que faz |
|---|---|
| `TROCA` | Mostra o build rodando, as trocas desde o boot e as cópias do WINE.DLL. |
| `TROCA KERNEL /d/TEMP/KRN.ELF /T` | Testa a troca do kernel: monta tudo e diz o que faria. Sem `/T`, troca. O ELF precisa vir do `make kernel_multiboot.elf` do `src13` (liga com `ld -q` e esconde as `.rela.*` do GRUB com `tools/relaoculta`; sem isso o próximo boot para em "ELF files with relocs are not supported yet"). |
| `TROCA WINE` | Os próximos EXEs do Windows usam o `D:\SODOS\DRV\WINE.DLL` do disco. Os que já rodam, inclusive este Claude Code, ficam na cópia antiga até fechar. |

Não use para mudanças de `struct` ou de tamanho de variável do kernel: aí, reinicie. Detalhes em `src13/docs/TROCA.md`.

O número do build (`ver`, `TROCA`, `\LOG\BOOT.LOG`) é a contagem de commits do código, o mesmo em toda máquina para o mesmo commit. Um kernel compilado com mudanças não commitadas sai como `build N+K`, e o K sobe a cada kernel novo em cima do mesmo commit. O `make kernel_multiboot.elf` atualiza o número sozinho (não precisa de `make version`). Ao citar um kernel num commit ou numa mensagem, use esse número (ex.: `build 240+3`). No clone raso do GIT.EXE a contagem vem uma vez da API do GitHub, então o primeiro build depois de um clone ou pull precisa de rede; sem ela o kernel sai como `build ?`.

## Cuidados

- `tecla` e `mouse` vão para a janela que tem o foco. Antes de digitar, confira com `janelas` ou clique na janela certa.
- No desktop, sem janela com foco, `{ESC}` fecha o WIN inteiro. `{CTRL+ALT+DEL}` leva o sistema para a tela 1 (Ctrl+F1); na tela 1 reinicia o computador. Não mande essas teclas sem necessidade.
- A pasta atual do `sodos` é a do SODOS, não a do Linux: use caminhos completos ou mande antes `sh /d/WSL/SODOS cd /d/PASTA`. Ela é a do desktop do WIN: o `command` aberto depois começa nela. Cada Command e cada programa do WIN tem a sua pasta, e o `cd` de um não muda a dos outros.
- O SODOS aceita nomes longos (VFAT LFN, em FAT16 e FAT32): os programas recebem até 31 caracteres, e um nome mais longo chega como o apelido 8.3 (`NOME~1.EXT`). Não é preciso encolher nomes para 8.3. O texto dos programas é em CP850. Para compilar: `sh /d/WSL/SODOS cc -I /d/ -L /d/ -o /d/PASTA/PROG.EXE /d/PASTA/PROG.C`.
- Mais detalhes no repositório do SODOS: `src13/docs/CONTROLE.md`.
