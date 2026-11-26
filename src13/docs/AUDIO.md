# AUDIO — som do SODOS

Som opcional, ligado e desligado como o WINE e o LINKEXT:

```
SET AUDIO=1          liga: carrega o AUDIO.DLL (driver HD Audio) e acha a placa
SET AUDIO=0          desliga: para o som e tira o DLL da memoria (driver nulo)
SET AUDIO=X.DLL      outro driver da mesma pasta (testes, drivers futuros)
```

Sem a variável (ou `AUDIO=0`) o driver atual é o **nulo**, que fica no kernel: nenhum DLL carregado, nenhuma memória reservada, nenhum acesso à placa, e no IRQ0 só o teste de uma variável. Os programas funcionam do mesmo jeito; o som vai para lugar nenhum, como o `NUL` do DOS.

O `AUDIO.DLL` é procurado em `PATHSODOS\DRV`, depois `D:\SODOS\DRV` e `Z:\SODOS\DRV`. Se a unidade ainda não está montada no `SET` (AUTOEXEC), ele tenta de novo no próximo som. Para ler um DLL novo do disco: `SET AUDIO=0` e `SET AUDIO=1`.

## Comandos

| Comando | O que faz |
|---|---|
| `AUDIO` | Estado: driver, placa, codec, saídas, taxas, volume, quem está tocando e o log do carregamento. |
| `AUDIO TESTE [hz] [ms]` | Tom de teste (padrão 440 Hz, 1 s). Volta na hora: o IRQ0 alimenta o tom. |
| `AUDIO VOL [0..100]` | Lê ou muda o volume da saída (amplificador do codec, nunca acima de 0 dB). |
| `PLAY arquivo.wav` | Toca um WAV PCM de 8 ou 16 bits, mono ou estéreo, de 8 a 192 kHz. |
| `PLAY /T` | Escala de teste (22050 Hz, mono, 8 bits), sem arquivo. |
| `HDAVERB no verbo [dado]` | Diagnóstico: manda um verbo cru ao codec em uso (hexadecimal) e mostra a resposta. `HDAVERB /T ms` espera sem prender o desktop. |

O `AUDIO.DLL` também grava no log (serial e comando `AUDIO`) o estado de todos os nós do codec ao carregar e no primeiro som.

Em `.BAT` de teste não use `PAUSE` dentro do WIN: ele lê o teclado em laço e congela o desktop até chegar uma tecla (foi o que pareceu um travamento em 25/11). Use `HDAVERB /T ms` ou um `PLAY` de um WAV de silêncio para esperar.

## Programas

Em `so.h` (exports do kernel a partir do build 61):

```c
int k_audio_open(int rate, int channels, int bits);  /* 0 ok; -1 parametros; -2 outro programa; -3 placa recusou */
int k_audio_write(const void* buf, int bytes);       /* espera lugar na fila (~200 ms); bytes aceitos ou <0 */
int k_audio_close(int wait);                         /* 1: espera tocar o resto; 0: corta */
int k_audio_pending(void);                           /* bytes ainda na fila (sincronizar video) */
int k_audio_volume(int pct);                         /* 0..100; pct < 0 so le; -1 sem placa */
int k_audio_beep(int hz, int ms);                    /* tom sem esperar */
int k_audio_info(char* out, int max);                /* texto do comando AUDIO */
```

Um programa por vez tem o som. Quando ele termina (ou é morto) sem `k_audio_close`, o kernel para a placa e libera o som. O `k_audio_write` dorme (futex) enquanto a fila está cheia: não gasta CPU.

O kernel converte o que o programa manda para o que o driver recebe (sempre 16 bits estéreo): 8 bits sem sinal, mono, e taxas que a placa não tem (vai a 48 kHz por interpolação linear, sem filtro). No notebook (Conexant), a placa só tem 48, 96 e 192 kHz: WAV de 44,1 kHz passa pela conversão.

## Como funciona

- `kernel/audio.c`: driver nulo, carga do DLL (`SET AUDIO`, `env.c`), conversão de formato, o tom e os exports. O estado fica numa `struct` com guardas nas pontas (TROCA KERNEL).
- `drivers/audio/hda.c` (`AUDIO.DLL`): driver HD Audio. Roda no contexto do kernel (como FAT32.DLL e LINKEXT.DLL), sem libc e sem importar nada; tudo vem em `struct audio_host` (`kernel/audio.h`).
  - Acha os controladores HDA no PCI (classe 04:03). Tenta primeiro os que não são HDMI de placa de vídeo (NVIDIA, ou ATI ao lado da GPU); o HDMI só tem pinos digitais e o amdgop liga a tela em modo DVI, sem áudio.
  - Reseta o controlador e fala com o codec pela CORB/RIRB; se a CORB não responde, usa os comandos imediatos.
  - Fica com o primeiro codec que tem saída analógica (alto-falante, fone, linha), acha o caminho pino → mixer/seletor → DAC de cada uma, liga a energia, o EAPD (amplificador externo) e o volume. Todos os DACs ouvem o mesmo fluxo. Com fone ligado (presence detect, olhado a cada 500 ms enquanto toca) o alto-falante cala.
  - Toca um fluxo de saída por DMA num buffer circular de 128 KB (32 entradas de 4 KB, nenhuma cruza 4 KB), em polling, sem interrupção.
  - AMD/ATI: snoop ligado no byte 0x42 do PCI e o CORBRP que volta a 0 sozinho; Intel: TCSEL = 0 (como o `hda_intel.c` do Linux).
- O buffer é circular: se ninguém escrevesse, a placa tocaria de novo o que já tocou. Enquanto há som, o IRQ0 (`irq.c`, com o BKL do mundo, a cada ~8 ms) chama o `service` do driver, que zera o que a placa já passou. Um programa que para de mandar amostras fica em silêncio, e 1,5 s depois a placa para (DMA desligado) e o IRQ0 volta a só testar a variável. O próximo `k_audio_write` liga de novo.
- Falta de som (o programa atrasou): a escrita pula para logo à frente da placa; o trecho perdido sai como silêncio.

## Testado (25/11/2026, build 61)

Notebook com Ryzen: controlador AMD `1022:15E3` em 04:00.6, codec Conexant `14F1:20D1`, alto-falante no nó 17 e fone no nó 16, os dois no DAC 10.

- `SET AUDIO=1` acha a placa; `SET AUDIO=0` descarrega (o DLL some do `DLLS`) e o `AUDIO` volta ao nulo; `SET AUDIO=1` de novo carrega.
- `AUDIO TESTE`: a fila fica em ~100 ms e a placa para sozinha depois do fim.
- `PLAY /T` (22050 Hz mono 8 bits) leva 2,79 s para 2,67 s de som; `PLAY` de um WAV de 3 s em 44,1 kHz estéreo leva 3,08 s: a placa consome no ritmo certo.
- `KILL` no meio do `PLAY`: a placa para e o som fica livre na hora.
- A lógica da fila (ordem, nada de som velho repetido, silêncio no fim, falta de som, recomeço) foi conferida num simulador na VM com uma placa falsa.

### Pendente: o alto-falante do notebook não toca

Tudo acima mostra a placa tocando, mas **não sai som do alto-falante** do notebook (subsistema PCI `2782:12C2`, codec Conexant SN6180 `14F1:20D1`). Já foi descartado, com `HDAVERB` e `.BAT`s em `D:\TEMP` (`SOMTESTE`, `SOMTEST2`, `SOMTEST3`):

- caminho do codec: DAC 10 em D0, fluxo 1, formato `0011`, sem mudo; pino 17 (alto-falante, config `91170110`) com saída e EAPD ligados;
- bipe interno do codec (nó 12, verbo `70A`): também mudo, então não é o fluxo de dados;
- os 8 GPIOs do codec em nível alto e em nível baixo;
- os pinos 1D (DAC 22) e 26 (DAC 23), que a BIOS marca como "nada ligado" (o Linux liga o 1D nos "Sirius" `2782:12C3`/`12C5`);
- energia em todos os nós (10 a 28), todos os DACs no mesmo fluxo e todos os pinos de saída ligados.

**O som saiu (25/11, 20:33):** depois de um boot no Linux (o SODOS ficou fora das 20:04 às 20:32), o `D:\TEMP\SOMTEST4.BAT` fez um reset do grupo de áudio (`HDAVERB 1 7FF 0`) seguido de `HDAVERB 1 705 0` (D0), e o alto-falante tocou; depois disso os testes antigos também tocaram. O despejo do codec ao carregar o driver era idêntico antes e depois do Linux.

Duas explicações cabem nisso, e um boot frio separa as duas:

1. o codec chegava do boot já em D0 e o `SET_POWER D0` do driver não mudava nada; o reset seguido de D0 faz o codec passar de fato pela sequência de ligar (o amplificador interno sobe nessa transição);
2. o Linux deixou ligado algo fora do codec (ACPI/EC ou GPIO da AMD), que sobrevive a reboot sem cortar a energia.

O `AUDIO.DLL` agora faz no init o reset do grupo de áudio e a energia D3 → D0 (`codec_power_cycle` em `drivers/audio/hda.c`), e configura tudo de novo depois. Falta também testar o fone (pino 16).

**Boot frio x boot vindo do Linux (25/11, noite).** O notebook é um Positivo VAIO VJFE69F11X (fabricante Emdoor, subsistema `2782`). O alto-falante toca no Linux e no SODOS depois de um reboot *quente* vindo do Linux (20:32, 21:57, 22:17); depois de cortar a energia fica mudo. Descartado, um por um:

- reset do codec (`7FF`) e D3 → D0 no grupo de áudio; EAPD de 0 para 1; os GPIOs do codec; os pinos 1D/26; energia em todos os nós; o codec idêntico ao do Linux tocando (alto-falante pelo DAC 11, como o Linux);
- os comandos que o Linux manda ao codec (rastreados com os eventos `hda` do kernel, `D:\TEMP\LINUX2\verbos_A.txt`): só verbos padrão;
- ACPI da placa (`_PS0` do `AZAL` só age com `APGE`/`ACGE`, que são 0 nesta BIOS); GPIOs de saída da AMD; ACP (o init do `snd_rn_pci_acp3x` repetido no SODOS); ciclo PCI D0 → D3hot → D0 do HDA;
- PCI de todas as funções do dispositivo, registradores do HDA e dos nós próprios da Conexant (1C, 1B, 28): **idênticos** entre frio e quente (`D:\TEMP\FOTOF.TXT` e `FOTOQ.TXT`, a "fotografia" que o driver grava).

O que **difere** é só a memória do **EC** (fora temperatura e ventoinha): bytes `0x12` (00 → FF), `0x2B` (D7 → 6E) e `0x2D` (00 → 03), que o ACPI nem nomeia. O EC tem energia própria e só esquece ao cortar a energia. Hipótese em teste: o EC solta o amplificador quando o sistema liga o **modo ACPI** (FADT: `SMI_CMD` ← `ACPI_ENABLE`), o que o Linux faz no boot ("ACPI: Interpreter enabled") e o SODOS nunca fez. O `AUDIO.DLL` de teste liga o modo ACPI (`acpi_enable_test`) e grava os três bytes antes e depois.

## Limites

- Só saída; sem microfone (no Ryzen os microfones internos ficam no ACP, outro dispositivo).
- Sem HDMI/DisplayPort.
- Codecs que precisam de ajustes específicos (coeficientes da Realtek, GPIO de amplificador, amplificadores inteligentes por I2C) podem ficar mudos: o `AUDIO` mostra o caminho escolhido para comparar com o `hda-analyzer`/`codec#` do Linux.
- A conversão de taxa não tem filtro: boa para voz, avisos e jogos; música fica melhor em 48 kHz.
