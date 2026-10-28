# Contrato de Boot do SODOS (BootInfo v1)

Este documento descreve o carregador BIOS em dois estágios, o layout físico e o
contrato `BootInfo` entregue ao kernel.  Ele é a referência para o carregador
BIOS atual e para o futuro carregador UEFI.

Arquivos envolvidos:

| Arquivo | Papel |
|---|---|
| `kernel/boot.asm` | Stage 1: 512 bytes, carrega o stage 2 e passa unidade/LBA. |
| `kernel/stage2.asm` | Stage 2: A20, E820, validação, carga do kernel e BootInfo. |
| `kernel/kernel_entry.asm` | Entrada 32 bits: valida BootInfo, entra em long mode, chama `kmain`. |
| `kernel/bootinfo.h` / `.c` | Estrutura versionada, offsets, validação e acesso global. |
| `kernel/linker.ld` | Kernel linkado/carregado em `0x01000000`. |
| `tools/validate_layout.sh` | Validação de tamanhos/endereços/sobreposições no build. |

## 1. Fluxo

```text
BIOS                                              UEFI (futuro)
 |                                                  |
boot.asm (stage 1, 0x7C00)                       sodos.efi
 |                                                  |
stage2.asm (0x7E00)                                |
 |  E820, validação, carga do kernel                |
 |  preenche BootInfo                               | preenche BootInfo
 +------------------------+-------------------------+
                          |
                     struct BootInfo v1
                          |
                  kernel_entry (BIOS) / entrada UEFI
                          |
                      kernel SODOS (x86-64)
                          |
                       kmain(BootInfo*)
```

O kernel C é único.  O que muda entre firmware é o carregador e o stub de
entrada; o contrato `BootInfo` e `kmain()` permanecem os mesmos.

## 2. Layout físico

| Endereço | Tamanho/uso |
|---|---|
| `0x00004FF0` | Parâmetros stage 1 → stage 2: `boot_drive` (u32), `kernel_lba` (u32). |
| `0x00005000` | `BootInfo` (128 bytes, fixo). |
| `0x00005200` | Mapa E820 (`64 × 24 = 1536` bytes, até `0x5800`). |
| `0x00007C00` | Stage 1 (setor 0 da imagem). |
| `0x00007E00` | Stage 2 (máximo 56 setores / `0x7000` bytes). |
| `0x0000FC00` | Topo da pilha real do stage 2 (cresce para baixo; stage 2 termina até `0xEE00`). |
| `0x00020000` | Bounce buffer do stage 2 (32 setores = 16 KiB). |
| `0x00058000` | Aplicação/initrd legado; slot de 32 KiB até `0x60000`. |
| `0x0007C000` | Porta NET1 (20 bytes). |
| `0x0007E000` | Bounce buffer da FAT (512 bytes). |
| `0x00080000` | Buffers de MBR/fdisk do kernel. |
| `0x00080800` | Buffer de formatação FAT (512 bytes, símbolo absoluto). |
| `0x0009FC00` | Topo da pilha do kernel. |
| `0x000A0000` | VGA/ROM e memória reservada. |
| `0x000B8000` | VGA texto. |
| `0x00100000` | Back buffer gráfico do kernel (até ~10 MiB). |
| `0x00A00000` | Buffers FAT do kernel (`0xA00000`–`0xA59000`). |
| `0x00A80000` | `kmalloc` do kernel (`0xA80000`–`0xC80000`). |
| `0x01000000` | **Kernel**: base de link e de carga (16 MiB). |
| `0x01200000` | Início típico do heap alto (`umalloc`), dinâmico após `__bss_end`. |
| `0x0FFE0000` | Limite do heap alto (identidade de 256 MiB montada por `kernel_entry`). |
| `0xE0000000` / `0xFD000000` | LFB Bochs/QEMU mapeado pelo kernel. |

O kernel não ocupa mais a memória baixa.  A carga em 16 MiB libera o antigo
intervalo de 288 KiB (`0x10000`–`0x58000`) e permite imagens de vários MiB.  O
build limita o kernel a `0x0E000000` (224 MiB) e valida que o fim do BSS cabe
abaixo da identidade de 256 MiB.

## 3. Layout da imagem (`os.img`)

| LBA | Conteúdo |
|---|---|
| 0 | `boot.bin` (stage 1, 512 bytes, assinatura `0xAA55`) |
| 1 .. N | `stage2.bin` (`N` setores, no máximo 56) |
| N+1 .. N+K | `kernel.bin` (`K = ceil(tamanho/512)`) |
| N+K+1 .. | `app.bin` |

A entrada 1 da tabela MBR descreve `start=1`, `count=N+K+A`.  O SODOS não monta
o drive 0 como FAT (C: é RAM; D: é `disk.img`), então essa partição é apenas
descritiva.  Os discos adicionais e a montagem de `D:` não mudam.

## 4. Stage 1 — `boot.asm`

Responsabilidades mínimas:

1. Configurar pilha em `0x7C00`, modo texto e salvar `DL` (unidade BIOS).
2. Gravar `{boot_drive, kernel_lba}` em `0x4FF0`.
3. Ler `STAGE2_SECTORS` setores do LBA 1 para `0x7E00`, usando `INT 13h AH=42h`
   (LBA estendido) ou CHS como fallback.
4. Saltar para `0x0000:0x7E00`.

O stage 1 não habilita A20, não carrega o kernel e não entra em protected mode.

Se o disco falhar, exibe `S1: disk error` e para.  `STAGE2_SECTORS` é calculado
pelo Makefile a partir de `stage2.bin`; o build falha se o stage 2 passar de 56
setores.

## 5. Stage 2 — `stage2.asm`

Ordem de execução:

1. Salva `DL` e recupera `kernel_lba` de `0x4FF4`.
2. Habilita A20 (`INT 15h AX=2401h` + porta `0x92`).
3. Verifica LBA estendido; usa CHS (63 setores/trilha, 16 cabeças) se preciso.
4. Zera `BootInfo` em `0x5000`.
5. Coleta E820 em `0x5200`, no máximo 64 entradas de 24 bytes.  Se E820 falhar,
   exibe `S2: E820 failed` e para.
6. Valida que `[KERNEL_LOAD, KERNEL_LOAD+KERNEL_RESERVED)` e o slot da aplicação
   estão inteiramente dentro de regiões E820 tipo 1 (utilizável).  Reservado,
   ACPI e buracos não são usados.  Falhas exibem mensagem clara.
7. Carrega o kernel em blocos de até 32 setores para `0x20000` e copia cada
   bloco para `KERNEL_LOAD` usando um pequeno modo protegido (o BIOS não alcança
   acima de 1 MiB).  A volta ao modo real usa um descritor de código 16 bits.
8. Carrega `app.bin` em `APP_LOAD` (`0x58000`) pelo mesmo caminho.
9. Preenche `BootInfo` e entra em protected mode com `EBX = 0x5000`, chamando
   `KERNEL_ENTRY`.

Mensagens de erro do stage 2:

| Mensagem | Causa |
|---|---|
| `S2: E820 failed` | BIOS sem E820 válido ou mais de 64 entradas. |
| `S2: kernel memory not usable (E820)` | Faixa do kernel não é RAM utilizável. |
| `S2: app memory not usable (E820)` | Slot da aplicação não é RAM utilizável. |
| `S2: disk read error` | Falha de leitura LBA/CHS. |

## 6. Contrato de entrada do kernel

O ponto de entrada continua sendo `kernel_entry` (símbolo do ELF, primeiro
endereço do `kernel.bin`).

Estado exigido na entrada:

| Item | Valor |
|---|---|
| Modo | 32 bits, protected mode, paging desligado |
| Interrupções | desligadas (`IF=0`) |
| A20 | ligado |
| `CS` | `0x08` (código 32 bits, base 0, limite 4 GiB) |
| `DS`, `ES`, `FS`, `GS`, `SS` | `0x10` (dados 32 bits, base 0, limite 4 GiB) |
| `ESP` | `0x009FC00`, alinhado a 16 bytes |
| `EBX` | endereço físico de `struct BootInfo` |
| `EAX` | `0x53444F53` (informativo) |
| `DF` | 0 (direção limpa) |
| `CR0.PG` | 0 |
| `CR3` | irrelevante |

`kernel_entry` valida `EBX`/magic, monta as tabelas de página de 2 MiB
(identidade de 0–256 MiB + LFB), entra em long mode e chama:

```c
void kmain(const struct BootInfo *bi);
```

Se o magic estiver errado, `kernel_entry` escreve `B` na COM1 e para.  Se o
CPU não tiver long mode, escreve `N` e para.

### 6.1 Estado depois de `kmain`

`kmain` roda com `cli`, zera o BSS (`__bss_start`..`__bss_end`), chama
`bootinfo_set`, inicializa a serial e o `mm`, e só depois configura IDT/PIC.
Nenhum serviço BIOS é usado pelo kernel.

## 7. `struct BootInfo` v1

Definição autoritativa: `kernel/bootinfo.h`.  Tamanho fixo de **128 bytes**,
little-endian, `__attribute__((packed))`.  O stage 2 usa os mesmos offsets.

| Offset | Tipo | Campo | Significado |
|---:|---|---|---|
| 0 | u32 | `magic` | `0x53444F53` (“SODS”). |
| 4 | u32 | `version` | `1`. |
| 8 | u32 | `size` | Tamanho da estrutura (128). |
| 12 | u32 | `firmware` | 0=desconhecido, 1=BIOS, 2=UEFI. |
| 16 | u64 | `kernel_phys` | Início físico do kernel. |
| 24 | u64 | `kernel_size` | Bytes úteis de `kernel.bin`. |
| 32 | u64 | `kernel_entry` | Endereço físico de `kernel_entry`. |
| 40 | u64 | `app_phys` | Aplicação/initrd, 0 se ausente. |
| 48 | u64 | `app_size` | Tamanho da aplicação/initrd. |
| 56 | u64 | `fb_addr` | Framebuffer linear, 0 se não fornecido. |
| 64 | u32 | `fb_width` | Largura em pixels. |
| 68 | u32 | `fb_height` | Altura em pixels. |
| 72 | u32 | `fb_pitch` | Bytes por linha. |
| 76 | u32 | `fb_bpp` | Bits por pixel. |
| 80 | u32 | `fb_format` | 0=none, 1=RGB, 2=BGR. |
| 84 | u32 | `mem_count` | Entradas válidas do mapa. |
| 88 | u32 | `mem_capacity` | Capacidade da área do mapa (64). |
| 92 | u32 | `flags` | Reservado; 0 nesta versão. |
| 96 | u64 | `mem_map_addr` | Endereço físico de `BootMemEntry[]`. |
| 104 | u64 | `boot_drive` | Unidade BIOS usada no boot. |
| 112 | u64[2] | `reserved` | Reservado; 0. |

### 7.1 Entrada do mapa

`struct BootMemEntry` tem 24 bytes, o mesmo formato de uma entrada E820:

| Offset | Tipo | Campo |
|---:|---|---|
| 0 | u64 | `base` |
| 8 | u64 | `length` |
| 16 | u32 | `type` (1=utilizável, 2=reservado, 3=ACPI reclaim, 4=ACPI NVS, 5=ruim) |
| 20 | u32 | `attr` (atributos estendidos ACPI; 0 se o BIOS não os fornecer) |

O mapa e a `BootInfo` precisam estar na identidade de 0–256 MiB (o BIOS os
coloca em `0x5000`/`0x5200`).  O kernel usa o mapa para escolher o heap alto:
começa em `align_up(max(__bss_end, kernel_phys+kernel_size), 2 MiB)` e termina
no fim da primeira região utilizável que caiba pelo menos 4 MiB, limitado a
256 MiB.  Nenhuma memória reservada é sondada ou escrita.

## 8. Framebuffer

No caminho BIOS, o stage 2 **não** configura VBE: o kernel inicializa o vídeo
como antes (`vga`/`visual`).  Por isso `fb_addr=0` e `fb_format=NONE`; campos de
framebuffer ficam prontos para um carregador UEFI preencher com GOP.

## 9. Build e validação

Constantes em `Makefile`:

```make
KERNEL_LOAD       := 0x01000000
KERNEL_MAX_SIZE   := 0x0E000000
APP_LOAD          := 0x00058000
APP_MAX_SIZE      := 0x00008000
STAGE2_LOAD       := 0x00007E00
STAGE2_MAX_SIZE   := 0x00007000
BOOTINFO_ADDR     := 0x00005000
E820_ADDR         := 0x00005200
IMAGE_MAX_SECTORS := 1048576
```

`tools/validate_layout.sh` roda antes de `os.img` e falha o build quando:

- `kernel.bin`/`stage2.bin`/`app.bin` estão vazios ou passam dos limites;
- `kernel_entry` ou `__bss_end` ficam fora da reserva do kernel;
- algum segmento `LOAD` do ELF começa antes de `KERNEL_LOAD` ou termina depois
  da reserva (isso pega o antigo buffer de formatação carregável em `0x80800`);
- `format_buf_region` deixa de ser símbolo absoluto em `0x80800`;
- BootInfo, mapa E820, stage 2, aplicação e kernel se sobrepõem;
- a imagem passa de `IMAGE_MAX_SECTORS`.

Testes negativos usados na implementação: base antiga `0x10000`, stage 2 acima
de `STAGE2_MAX_SIZE` e app acima do slot — todos falham com mensagem explícita.

## 10. UEFI (etapa futura, não implementada)

O contrato permite um `sodos.efi` sem duplicar o kernel C:

1. O firmware UEFI carrega `sodos.efi`, que localiza/abre o `kernel.bin` e o
   initrd no sistema de arquivos EFI.
2. `sodos.efi` aloca o kernel em `0x01000000` (ou em endereço compatível),
   preenche `BootInfo` com `firmware=UEFI`, `mem_map_addr/mem_count` no formato
   `BootMemEntry` e os campos de framebuffer do GOP.
3. O mapa UEFI (`EfiConventionalMemory`, `EfiLoaderCode/Data`,
   `EfiBootServicesCode/Data`, `EfiRuntimeServices*`, `EfiACPI*`, `EfiReserved`)
   é convertido para os tipos `BOOTINFO_MEM_*`; regiões de boot services só
   viram utilizáveis depois de `ExitBootServices`.
4. `sodos.efi` encerra boot services, desliga interrupções e chama a entrada
   64 bits do kernel.  O stub UEFI pode montar a mesma identidade de 256 MiB e
   chamar `kmain(BootInfo*)`; o `kernel_entry` 32 bits do BIOS não é obrigatório
   no caminho UEFI, mas `kmain` e `BootInfo` são os mesmos.
5. Nenhum driver, aplicação ou formato de executável precisa mudar.

O `BootInfo` é versionado: campos novos podem ser adicionados apenas usando
espaço reservado ou com `version=2`; offsets existentes da v1 não mudam.

## 11. Como testar

```sh
cd src13
make                 # gera stage1, stage2, kernel e os.img
make run             # QEMU BIOS + os.img + disk.img + disk2.img (D:)
make run-log         # grava COM1 em qemu_serial.log
make check-layout    # somente a validação de layout
```

No shell do SODOS:

```text
free        # heap baixo 0xA80000-0xC80000 e heap alto após o kernel
dir d:      # unidade D: (disk.img)
visual      # modo gráfico; ESC volta ao texto
```

O log serial deve mostrar `BootInfo v1, firmware=BIOS`, o endereço
`kernel=0x01000000`, `e820=` e `OS Ready.`/`shell ready`.
