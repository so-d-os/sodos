# Comandar o SODOS por linha de comando

Comandos para o Claude (o Claude Code roda no WINE e o Bash dele é a VM Linux do WSL), a API HTTP, os `.BAT`, o AGENT2 e a PONTE abrirem programas, tirarem print e usarem mouse e teclado. Cada um chama a mesma rotina que o usuário aciona com a tecla ou com o mouse de verdade.

| Comando | O que faz |
|---|---|
| `COMMAND [programa args]` | Abre um Command no WIN e roda a linha nele, como se ela tivesse sido digitada ali. Programa de texto de tela cheia (TOP, AGENT2, MC, EDIT) ganha tela e teclado próprios. Sem linha, só abre o Command. |
| `PRTSC [/T] [arquivo.png]` | Print da tela que está à vista (WIN ou tela de texto), pela mesma rotina da tecla PrtSc: `Z:\SAVE\SCREENSHOT\printscreen-AAAAMMDD-HHMMSS.png`, mais a cópia em `D:\TEMP`. Mostra os dois caminhos. `/T` tira a tela de texto desta tela do DOS; `arquivo` grava mais uma cópia nesse caminho. |
| `TECLA texto {TECLA}...` | Digita na janela com foco pelo caminho do teclado (fila do PS/2, layout do `KEYB`, teclas mortas para acentos). `{ENTER} {TAB} {ESC} {BS} {DEL} {INS} {HOME} {END} {PGUP} {PGDN} {CIMA} {BAIXO} {ESQ} {DIR} {F1}..{F12} {ESPACO} {PRTSC} {WIN}`, combinações `{CTRL+C} {ALT+TAB} {CTRL+F12} {SHIFT+TAB}`, repetição `{TAB 3}` e caractere literal `{>} {|} {<} {{} {}}`. Aceita texto em UTF-8 (do curl) ou em CP850. |
| `MOUSE x y [CLICK\|DIREITO\|DUPLO\|MEIO\|DESCE\|SOBE]` | Move o cursor do WIN e clica. `MOUSE CLICK` clica onde o cursor está; `MOUSE POS` mostra a posição. Para arrastar: `DESCE`, depois `MOUSE x y`, depois `SOBE`. Sem argumentos, `MOUSE` continua sendo o teste do mouse (interativo). |
| `JANELAS` | Lista as janelas do WIN: título, x, y, largura e altura; `*` marca a que tem o foco. |
| `WEBCAM [arquivo] [/L] [/SHOW] [/W:largura \| /R:LxA] [/S:quadros]` | `/SHOW` abre o `D:\WEBCAMV.EXE` (`utils/webcamv.c`): uma janela do WIN com a webcam ao vivo (YUY2 até 640x480; `/R:1280x720` usa MJPEG); Esc fecha, F espelha. Sem `/SHOW`: foto da webcam USB (UVC, classe 0E, USB 2): `D:\TEMP\WEBCAM.JPG` sem arquivo. MJPEG vira JPEG (com as tabelas de Huffman padrão quando a câmera não manda); câmera só com YUY2 grava BMP. `/L` lista formatos, tamanhos e bandas; `/W` é a largura máxima (padrão 640); `/R:640x360` pede o tamanho exato; `/S` são os quadros descartados antes (padrão 10, para a exposição automática). Leva ~1 s; o stream roda numa thread do kernel com anel de eventos próprio (interruptor 1 do xHCI), então o mouse e o resto do USB seguem normais. Programas usam `k_cam_modes`, `k_cam_open`, `k_cam_frame` e `k_cam_close` (`so.h`; teste: `utils/camtest.c`). `USB WEBCAM ...` é o mesmo comando (vale depois de um TROCA, que não registra comandos novos). |

`COMMAND` e `TECLA` recebem a linha inteira, antes de o shell separar `|`, `>` e `<`. Assim `command make > log.txt` roda o redirecionamento dentro do Command, e `tecla a>b` digita `a>b`.

## Do Claude (WINE + WSL)

Na VM Linux, a API do SODOS responde em `10.0.3.2:8797`:

```sh
sh /d/WSL/SODOS prtsc                    # print; o PNG fica em /d/TEMP/...
sh /d/WSL/SODOS command /d/DEVELOP/AGENT2/AGENT2.EXE
sh /d/WSL/SODOS 'tecla oi{ENTER}'
sh /d/WSL/SODOS mouse 640 400 click
curl -s -o /tmp/tela.png 'http://10.0.3.2:8797/win/printscreen.png?scale=2'
```

`D:\WSL\SODOS` manda a linha para o `/exec` da API. Ele troca `/d/x/y` (e o `/d/...` que o BASH.EXE do Claude Code põe no lugar de `D:\...`) por `D:\x\y`, sem mexer em opções como `/w`; escreva os caminhos como `/d/...`. O PNG pode ser lido direto pelo Read do Claude Code (`D:\TEMP\...png`) ou por `/d/TEMP/...` na VM.

### Por que a API não respondia da VM

1. O NAT da VM (`wsl/vmm/nat.c`) só respondia ao ping em 10.0.3.2 e descartava o TCP.
2. O SODOS não tinha loopback: um pacote para o próprio IP saía pela placa de rede, que não o devolve.
3. Quando um pedido chegava, ele rodava dentro de quem tivesse chamado o `nic_poll`, inclusive a thread da CPU da VM, que lê a fila do NAT. Lá um comando mexia na tela a partir de outra CPU.
4. O print pela API ou pela PONTE (que roda numa tela do DOS ao fundo) falhava com "WIN não está na tela": `win_screen_size` responde pela tela de quem chama, não pela que está à vista.

### O que mudou

- `nat.c`: TCP para 10.0.3.2 vai ao IP do próprio SODOS, e a resposta volta para a VM com origem 10.0.3.2.
- `net.c`: loopback. O que vai para o nosso IP entra na própria pilha (`loop_out`/`loop_drain`) em vez de sair pela placa. Na espera pelo ACK (`sv_wait_una`), o BKL é solto (`kt_bkl_breathe`) para a VM poder entregar o pacote.
- `httpd.c`: os pedidos só são atendidos no laço do WIN ou numa tela do DOS, nunca numa thread nem numa tarefa do WIN. A saída do `/exec` passou de 4,5 KB para 32 KB.
- `prtscr.c`: o print olha a tela à vista (`win_shown_size`).

O Claude Code do SODOS lê `D:\WINE\USERS\SODOS\.claude\CLAUDE.md`, que explica tudo isso a ele. A fonte é `docs/CLAUDE-SODOS.md`: o `make disk-files` (e portanto o `make send`) copia o arquivo para lá em toda instalação, sobrescrevendo o que houver. Mude o texto no repositório, não no D:. No repositório ele não se chama `CLAUDE.md` porque o Claude Code do Linux carregaria o arquivo como instrução dele ao mexer em `src13/docs`.

## Limites

- O `/exec` roda no laço do WIN, e o WIN fica parado até o comando acabar. Para programas longos ou interativos use `COMMAND`: a saída fica na janela, e um `PRTSC` mostra o resultado. A PONTE (`develop/PONTE`) continua servindo para rodar um comando longo e receber a saída.
- O servidor HTTP atende uma conexão por vez: os pedidos vão em sequência.
- Uma linha tem até 240 caracteres. A fila do `TECLA` guarda 4 KB (cerca de 500 teclas); o que passar disso dá erro, e é só repetir quando a fila esvaziar.
- `TECLA` e `MOUSE` vão para quem tem o foco, igual ao teclado e ao mouse de verdade. No desktop, `{ESC}` fecha o WIN.
- Precisa do kernel novo e do `WSL.EXE` novo (`D:\WSL.EXE`, o serviço da VM). Com o kernel antigo, a conexão para 10.0.3.2 simplesmente não abre.
