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
