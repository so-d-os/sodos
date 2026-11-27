# WSL no SODOS (`WSL.EXE`)

O "WSL2" do SODOS. Ele lê o Ubuntu do WSL2 direto do SSD do Windows e sobe
esse mesmo Ubuntu numa máquina virtual AMD-V, usando o kernel do próprio WSL.

```
WSL list                    distros e kernel achados nas partições NTFS
WSL info                    detalhes da distro (vhdx, ext4, os-release)
WSL ls /home                lista uma pasta do Ubuntu
WSL cat /etc/os-release     mostra um arquivo
WSL cp /home/eu/a.txt D:\A.TXT   copia do Ubuntu para o SODOS
WSL run                     sobe o Ubuntu (bash como root no console)
```

Use `-d NOME` para escolher a distro (prefixo do nome da pasta, por exemplo
`-d {b4b8`). `exit` no bash desliga a VM e volta ao prompt do SODOS;
**Ctrl+]** encerra na hora.

O console é um terminal `TERM=linux` (`wsl/vmm/term.c`): cores ANSI, cursor,
apagar e rolar, UTF-8 para CP850 e setas/F1-F12/Home/End/PgUp/PgDn. Enquanto a
VM roda, o teclado fica "cru" (`k_kbd_raw`): Ctrl+C e as outras Ctrl+letra vão
para o Linux em vez de parar o `WSL.EXE`. Com a saída desviada (WIN, API), as
sequências são retiradas e só o texto passa.

## Imagem própria (PC sem Windows)

Num PC só com Linux (sem NTFS) o WSL usa a distro **SODOS-Debian**, em
`D:\WSL\` (`SET WSLDIR` muda a pasta), que vira a padrão quando existe:

```
D:\WSL\DEBIAN.IMG   ext4 com Debian 12 mínimo (bash, git, curl, python3, ...)
  ou DEBIAN.000, DEBIAN.001...   a mesma imagem em partes (ganham do .IMG)
D:\WSL\VMLINUZ      kernel (o do Linux que gerou a imagem)
D:\WSL\INITRD.IMG   initrd pequeno: busybox + virtio_blk, monta /dev/vda
```

**Partes.** O `k_fsize` do SODOS devolve `int` (arquivo < 2 GB) e o 9p grava
o arquivo inteiro de uma vez, então a imagem pode vir em partes `DEBIAN.000`,
`.001`... (até 64; cada uma menos a última com tamanho múltiplo de 2 MB). O
`WSL list` mostra quantas são. `/etc/sodos-imagem` na VM diz de quando é a
imagem e o que ela tem.

**Imagem com as ferramentas de compilação** (gcc 12, binutils, libc6-dev,
make, nasm; feita em 26/11/2026). A camada de escrita some a cada boot, então
o que se instala na VM não fica. Para levar o que está na VM para uma imagem
nova, sem rede:

```
sh /d/SOURCE/SODOS/src13/wsl/scripts/nova-imagem.sh /d/WSL/NOVA
```

Ele extrai o `D:\WSL\FERRAM.TGZ` se faltar o gcc, congela o `/`
(`fsfreeze`), copia o `/dev/vda` em partes de 128 MB para a pasta, confere o
sha256 de cada parte relendo pelo 9p e copia `VMLINUZ` e `INITRD.IMG`. Uns
2 a 5 minutos. Para testar sem mexer no serviço, numa VM à parte (em outra
CPU, sem rede, desliga sozinha):

```
command D:\WSL.EXE run -D D:\WSL\NOVA -n -p 6 -t 180 -a "sodos-wsl:~#" -x "sh //d/TEMP/TESTE.SH\n"
```

O `TESTE.SH` grava o resultado num arquivo em `/d/TEMP` e termina com `sync`
e `/usr/local/bin/busybox poweroff -f`. Pelo `sh /d/WSL/SODOS`, escreva
`//d/...` dentro do `-x`: o script troca `/d/x` por `D:\x`, mas não `//d/x`.

Para usar: mover as partes para `D:\WSL` (o serviço com o `DEBIAN.IMG` não é
afetado) ou `SET WSLDIR=`, depois `SET WSL=0`, esperar o `WSL status` dizer
"servico parado" e `SET WSL=1`. O `SET WSL=1` só sobe o serviço quando o
valor muda, então ele não pode vir antes de o antigo parar. A PONTE faz essa
troca sem depender da VM. Para outros pacotes: `apt-get install` na VM (lista e cache em
`/tmp`, porque a camada é de 512 MB), depois `nova-imagem.sh` de novo.
Recriar do zero num Debian 12: `build-image.sh` (abaixo), que já inclui as
ferramentas e grava em partes.

Gerada sem root por `sh wsl/scripts/build-image.sh` (Linux Debian 12 com o
mesmo kernel; `MB=` tamanho; `OUT=` outra pasta; sai em partes de 128 MB,
ou `PARTS=0` para um `DEBIAN.IMG` só, menor que 2048 MB porque o `k_fsize`
do SODOS é `int`; já vem com gcc, binutils, libc6-dev, make, nasm e
e2fsprogs). No Ubuntu do WSL2 (sem `/boot`) use o kernel do próprio WSL:
`KIMG="/mnt/c/Program Files/WSL/tools/kernel" sh wsl/scripts/build-image.sh`
(virtio e 9p vêm embutidos nele; a chave do Debian, o busybox e o cpio vêm
por `apt-get download` em `~/wsltools`). O script desempacota os pacotes com
`mmdebstrap --mode=chrootless --variant=extract` dentro do `fakeroot`, grava
o ext4 com `mke2fs -d` num `unshare -r` e termina a instalação (`dpkg -i`)
num boot da própria imagem no QEMU. O init é `/sbin/sodos-init`: monta
`/proc` etc., carrega o `virtio_net`, configura a rede pelo `sodos.ip=` da
linha do kernel e roda o bash no console.

`WSL list/info/ls/cat/cp/run` usam a imagem; `-d NOME` escolhe uma distro do
Windows. `wsl --list [--verbose]` (sintaxe do `wsl.exe` do Windows) responde
em UTF-16LE, como o original. A imagem é lida em blocos de 2 MB guardados em
RAM alta; o que o Linux grava fica na camada em RAM, como no vhdx.

## Ao fundo: SET WSL=1, WSL exec e o bash do Claude Code

`SET WSL=1` sobe `WSL.EXE run --service` na tela **Ctrl+F10**: a VM fica
ligada, o console do Linux aparece no Ctrl+F10 (o `exit` do bash só abre
outro) e `SET WSL=0` desliga. Comandos de qualquer tela ou programa:

```
WSL exec uname -a          roda no Linux e devolve a saída e o código
WSL status                 serviço, agente e a serial de comandos
D:\WINE\BIN\BASH.EXE -c "cmd"   o mesmo WSL.EXE com o nome BASH.EXE: o bash
                           do Claude Code (-c, -l, -lc, --login)
```

Se o serviço não estiver ligado, o primeiro `WSL exec`/`BASH.EXE` faz o
`SET WSL=1` e espera o Linux subir (~8 s no QEMU). Por dentro: o kernel
guarda a caixa de comandos (`wslbox.h`, memória `k_malloc_shared`) no
registro de nomes (`k_svc_set "WSL"`); a thread da CPU da VM passa os
pedidos pela segunda serial (ttyS1, `vmm/chan.c`) ao
`/usr/local/sbin/sodos-agent` (Python) da imagem, que roda `bash -c` e
devolve stdout, stderr e o código. Ainda sem o D:/Z: dentro do Linux: o
comando roda em `/mnt/<unidade>/...` (a pasta atual do SODOS).

**Só a imagem SODOS-Debian tem o agente.** Sem `D:\WSL\DEBIAN.000` (ou
`DEBIAN.IMG`), `VMLINUZ` e `INITRD.IMG`, o `SET WSL=1` sobe a distro do
Windows (NTFS) só como console: o agente nunca diz READY. O `WSL exec` e o
`BASH.EXE` agora dizem isso na hora (antes esperavam 120 s cada um). Num PC
com Windows a imagem também precisa estar na pasta: ela não sai do NTFS.

**O bash do Claude Code no WINE.DLL** (`wine/dll/bashbox.c`): quando um
programa do Windows abre o `bash.exe` (o WSL.EXE com esse nome) e o serviço
está ligado ou pedido (`SET WSL=1`), o WINE.DLL não roda o `BASH.EXE`: uma
thread do próprio processo faz o mesmo pedido à caixa e escreve a saída no
pipe enquanto o comando roda. Antes o `BASH.EXE` ocupava a tarefa principal
do processo (só ela faz `k_exec`) até o fim do comando; qualquer outro
`CreateProcess` (git, o `rg` do Grep, outro bash) esperava a vez e desistia em
60 s com `ERROR_NOT_SUPPORTED`, o "ENOTSUP ... uv_spawn" do Claude Code.
Agora até 8 comandos correm juntos (vagas da caixa) e o `TerminateProcess`
do pai (tempo esgotado, Esc) manda o agente matar o comando.
O agente das imagens até 27/11/2026 não matava nada no `K`: ele nasce com
`&` do `sodos-init`, o SIGINT fica ignorado e os filhos herdam; o de agora
volta o SIGINT ao padrão nos filhos e, se o comando não sair, manda SIGTERM
(2 s) e SIGKILL (mais 3 s). Imagens velhas: refazer com o `build-image.sh`.
`SET WINE_BASHBOX=0` volta ao `BASH.EXE` na tarefa principal. Teste:
`wine/tests/bashpar.c` (`BASHPAR.EXE`: um bash lento, um rápido que volta
antes dele e um morto pelo `TerminateProcess`).

### As unidades do SODOS no Linux (virtio-9p)

O `WSL.EXE` tem um servidor 9P2000.L (`vmm/p9.c`, virtio-9p, tag `sodos`):
o `sodos-init` monta em `/mnt/sodos` e cria `/mnt/d`, `/mnt/z`, `/mnt/u`...
(uma por unidade montada), como no WSL. Leitura, escrita (o arquivo aberto
fica em RAM e vai inteiro ao disco no close/fsync, porque o `k_fsave` grava
o arquivo todo), criar, apagar, renomear, `mkdir`/`rmdir`, `df` e `git`
funcionam. Datas são as do FAT (sem mudar mtime), dono root, sem links.
O FAT não guarda o bit x: arquivo sem extensão no nome (`link64`,
`relaoculta`, `build/cc`) aparece como 0755, o resto como 0644, e o `chmod`
não muda nada. Os repositórios usam `core.filemode=false`, então o git não vê
diferença de modo.
O `BASH.EXE` traduz caminhos do Windows no comando: `D:\WINE\TEMP` vira
`/d/WINE/TEMP`, como no Git Bash (o Claude Code passa caminhos assim).
`BASH.EXE --version` responde na hora, sem subir a VM: o Claude Code confere
o shell assim, com 1 s de prazo.

O Claude Code 2.1.3xx só aceita como shell um caminho com `bash` em
minúsculas, vindo de `CLAUDE_CODE_SHELL` ou `SHELL` (o `CLAUDE_CODE_GIT_BASH_PATH`
só passa a checagem da partida). Sem isso todo Bash dá "No suitable shell
found". O `RODACLAUDE.BAT` faz:

```
SET CLAUDE_CODE_GIT_BASH_PATH=D:\WINE\BIN\bash.exe
SET CLAUDE_CODE_SHELL=D:\WINE\BIN\bash.exe
SET CLAUDE_CODE_USE_POWERSHELL_TOOL=0
SET WINEENV=CLAUDE_CODE_GIT_BASH_PATH;CLAUDE_CODE_SHELL;CLAUDE_CODE_USE_POWERSHELL_TOOL
```

## Como funciona

```
SSD NVMe (cru, só leitura: k_nvme_raw_read)
  └─ GPT → partição NTFS (C:)                       wsl/ntfs.c
       ├─ Program Files\WSL\tools\kernel  (bzImage do WSL2)
       └─ Users\<u>\AppData\Local\wsl\{guid}\ext4.vhdx
            └─ VHDX dinâmico (BAT, blocos)          wsl/vhdx.c
                 └─ ext4 (o / do Ubuntu)            wsl/ext4.c
```

- **Leitura** (`list/info/ls/cat/cp`): NTFS com runlists esparsas e
  `$ATTRIBUTE_LIST`, VHDX v1 e ext4 (extents, 64bit, htree lido como pasta
  linear, dados inline, links). Nada disso grava no disco do Windows.
- **`run`** (`wsl/vmm/`): o SODOS roda em ring 0 com memória em identidade,
  então o próprio `WSL.EXE` executa `VMRUN`. A VM é um PC ISA simples, para não
  precisar de ACPI nem de APIC:
  - 8259 PIC, 8254 PIT (tick e calibração do TSC), 16550 em 0x3F8 (console
    `ttyS0`) e CMOS RTC;
  - PCI tipo 1 com uma ponte e um **virtio-blk moderno** (BAR0 MMIO, INTx na
    IRQ 5). O kernel do WSL não tem `VIRTIO_MMIO_CMDLINE_DEVICES`, mas tem
    `VIRTIO_PCI`. O MMIO cai em NPF e um decodificador de `mov` trata o acesso;
  - paginação aninhada (NPT) com páginas de 2 MB. A RAM da VM fica acima de
    4 GB (`k_hmalloc`);
  - FPU/AVX trocada com XSAVE dentro do CLGI e XCR0 da VM emulado (XSETBV).
  - O kernel sobe pelo protocolo de 64 bits (`boot_params`, e820) com
    `nolapic acpi=off nokaslr root=/dev/vda`.
- **Rede** (`wsl/vmm/nat.c`): um virtio-net (00:02.0, IRQ 11) ligado a um NAT
  para o IP do SODOS, como o NAT do WSL2. Funciona também no WiFi, porque não
  faz ponte com outro MAC. A VM é 10.0.3.15/24 (pelo `ip=` do kernel, sem
  cliente DHCP); o "roteador" 10.0.3.2 responde ARP e ping; o DNS é o mesmo
  do SODOS (`/etc/resolv.conf` sai de `/proc/net/pnp`). Cada fluxo TCP/UDP, e
  cada eco ICMP, usa uma porta do SODOS entre 30000 e 39999. O kernel desvia
  as respostas para essas portas (`net_tap_*` em `net/net.c`, exportado como
  `k_net_tap_*`). Só IPv4; não aceita conexões de fora para a VM. `-n` sobe
  sem rede.
- **Disco da VM**: as leituras vêm do vhdx. **As escritas ficam numa camada em
  RAM e somem quando a VM termina** (`-w MB`, padrão 512). O `ext4.vhdx` do
  Windows nunca é alterado.
- **init**: o `/init` das distros WSL é o binário da Microsoft (precisa de
  Hyper-V). Por isso o padrão é `/bin/bash`, que antes monta `/proc`, `/sys`,
  `/dev/pts`, `/run` e `/tmp`. O shell PID 1 não é o seu bash: ele roda o bash
  como filho no `/dev/ttyS0` (`setsid -w -c`, com job control) e, quando o bash
  sai, faz `poweroff`. Se o init morresse, o Linux daria "Attempted to kill
  init"; `panic=-1` fica de rede de segurança (panic = reboot = a VM para). Um
  `/init` que seja script (`#!`) é usado como está, e `-i /sbin/init` sobe o
  systemd.

### Opções do `run`

| opção | padrão | |
|---|---|---|
| `-m MB` | 1024 | RAM da VM (128..3072) |
| `-w MB` | 512 | teto da camada de escrita em RAM |
| `-i INIT` | `/bin/bash` | programa init (`/sbin/init` = systemd) |
| `-k CAMINHO` | o do WSL | kernel: caminho no NTFS ou arquivo `D:\...` |
| `-c "params"` | | parâmetros extras do kernel |
| `-p CPU` | a última | CPU do SODOS dedicada à VM |
| `-t s` | sem limite | encerra depois de s segundos |
| `-x "texto"` | | digita no console (`\n` = Enter) |
| `-a "texto"` | | espera este texto na saída antes do `-x` |
| `-v` | | mostra portas/MMIO sem dispositivo e o total de saídas |
| `-n` | | sem rede (sem a placa virtio-net) |
| `-D PASTA` | `SET WSLDIR` ou `D:\WSL` | pasta da imagem própria só nesta VM |

O serviço (`SET WSL=1`) sobe com a linha fixa `WSL.EXE run --service`; para
dar opções a ele, `SET WSLOPT=-m 2048 -w 1024` antes do `SET WSL=1` (vão
depois das da linha; aspas juntam um argumento: `-c "a b"`). A tela Ctrl+F10
mostra `WSL: SET WSLOPT=...` quando elas valem.

## Requisitos e limites

- CPU AMD com SVM e NPT (Intel VT-x ainda não). Se o WSL2 roda no Windows, o
  SVM já está ligado no BIOS.
- O Windows num SSD **NVMe**; o SODOS usa o mesmo SSD da partição SODOSHD, ou
  o primeiro NVMe se ela não existir. **BitLocker** não é suportado.
- Um Windows que hibernou ou usou o "Inicialização rápida" deixa o NTFS sujo.
  A leitura costuma funcionar, mas o que mudou depois do último desligamento
  completo pode não aparecer. Se o vhdx avisar "log pendente", o WSL não foi
  fechado direito (`wsl --shutdown` antes de reiniciar resolve).
- Um processador, rede só IPv4 de saída, sem compartilhamento de arquivos com o
  SODOS (use `WSL cp`).

## Build e teste (no WSL)

```
sh wsl/scripts/build-wsl.sh        # wsl/build/WSL.EXE (+ KERNEL.IMP com os exports novos)
sh wsl/scripts/make-testdisk.sh    # wsl/build/windisk.img: GPT+NTFS com kernel e vhdx busybox
sh wsl/scripts/qemu-test.sh 'D:\WSL.EXE list' \
   'D:\WSL.EXE run -t 60 -m 512 -i /bin/sh -a "#" -x "uname -a; poweroff -f\n"'
```

O `make-testdisk.sh` baixa `ntfs-3g`, `ntfs-3g-dev` e `busybox-static` com
`apt-get download` (sem root) para `~/wsltools`. Também gera
`wsl/build/host_wsl`, que roda os comandos de leitura no Linux contra a imagem.

O teste no QEMU usa KVM com SVM aninhado (`-cpu host,+svm`), o disco de teste
como NVMe e um `disk.img` copiado. A saída da VM vai para
`wsl/build/qemu-serial.log`.

O kernel precisa ser o novo, porque `kernel/nvme.c` ganhou a leitura crua do
SSD inteiro (`nvme_raw_read`, exportada como `k_nvme_raw_read`).
