# Documentação do Sistema Operacional (kernel x86-64 freestanding)

Kernel 64-bit educacional para x86-64, sem libc (freestanding), com shell, disco ATA, partições MBR, FAT12/FAT16, VGA texto/gráfico e tratamento de exceções/IRQs.

> **Boot atual:** o kernel é linkado e carregado em `0x01000000` por um carregador BIOS de dois estágios. O contrato `BootInfo` v1, o mapa E820 e a interface para um futuro carregador UEFI estão em [`BOOT.md`](BOOT.md). Onde este documento divergir do layout de boot, `BOOT.md` prevalece.

---

## 1. Mapa de memória (endereços físicos)

| Endereço        | Tamanho / Uso |
|-----------------|----------------|
| `0x00004FF0`    | Parâmetros stage 1 → stage 2 (`boot_drive`, `kernel_lba`) |
| `0x00005000`    | `BootInfo` v1 (128 bytes) |
| `0x00005200`    | Mapa E820 (até 64 entradas de 24 bytes, até `0x5800`) |
| `0x00007C00`    | Stage 1 (boot sector, 512 bytes) |
| `0x00007E00`    | Stage 2 (carregador BIOS; até 56 setores) |
| `0x00020000`    | Bounce buffer do stage 2 (32 setores = 16 KiB) |
| `0x00058000`    | Slot da aplicação/initrd (32 KiB até `0x60000`) |
| `0x0007C000`    | Porta NET1 (20 bytes) |
| `0x0007E000`    | FAT bounce (512 bytes) |
| `0x00080000`    | Buffer MBR (512 bytes) — leitura/escrita de partições |
| `0x00080200`    | Buffer de saída do `fdisk list` (512 bytes) |
| `0x00080400`    | Buffer de verificação do `write_mbr` (512 bytes) |
| `0x00080600`    | Buffer string do drive no `fdisk list` (32 bytes) |
| `0x00080800`    | Buffer de formatação FAT (512 bytes; símbolo absoluto, fora do kernel.bin) |
| `0x00090000`–`0x000A0000` | Região da pilha do kernel (evitar buffers de disco aqui) |
| `0x0009FC00`    | Stack pointer inicial (kernel_entry) |
| `0x000A0000`    | Framebuffer VGA modo 13h (320×200, 256 cores) — 64 KB |
| `0x000B8000`    | Memória de texto VGA 80×25 (2 bytes por célula) |
| `0x00100000`    | Back buffer gráfico do visual.c (até ~10 MiB) |
| `0x00A00000`    | FAT do kernel: boot sector, tabela e root dir (até `0xA59000`) |
| `0x00A80000`    | Heap do kernel (`kmalloc`), 2 MiB até `0xC80000` |
| `0x01000000`    | **Kernel** (16 MiB): `.text`, `.rodata`, `.data`, `.bss` |
| `0x01200000`    | Início típico do heap alto (`umalloc`), escolhido pelo E820 após o kernel |
| `0x0FFE0000`    | Limite do heap alto (identidade de 256 MiB) |
| `0xE0000000` / `0xFD000000` | LFB Bochs/QEMU mapeado pelo kernel |

Regras do driver de disco em `kernel.c`: buffers devem ser não-NULL, `>= 0x1000`, dentro de `[0x60000, 0x100000)` e **não** em `[0x90000, 0xA0000)` (pilha).

---

## 2. Portas de I/O (hardware)

### 2.1 PIC 8259 (Programmable Interrupt Controller)

| Porta   | Leitura/Escrita | Uso |
|---------|------------------|-----|
| `0x20`  | PIC1 Command    | ICW1/OCW2 (init, EOI) |
| `0x21`  | PIC1 Data        | ICW2/3/4, máscara IRQ0–7 |
| `0xA0`  | PIC2 Command     | ICW1/OCW2 (init, EOI) |
| `0xA1`  | PIC2 Data        | ICW2/3/4, máscara IRQ8–15 |

Após remap: IRQ0–7 → vetores `0x20`–`0x27`, IRQ8–15 → `0x28`–`0x2F`. EOI: escrever `0x20` em Command do PIC que atende a IRQ (para IRQ ≥ 8, EOI nos dois).

### 2.2 ATA (IDE) PIO

**Primary bus (drive 0 = master, 1 = slave):**

| Porta   | Uso |
|---------|-----|
| `0x1F0` | Data (16-bit) |
| `0x1F1` | Error/Features |
| `0x1F2` | Sector count |
| `0x1F3` | LBA low |
| `0x1F4` | LBA mid |
| `0x1F5` | LBA high |
| `0x1F6` | Drive/head (0xE0 master, 0xF0 slave; bits 0–3 = LBA 24–27) |
| `0x1F7` | Command (0x20 read), Status (read) |

**Secondary bus (drive 2, 3):** mesmos offsets a partir de **`0x170`** (data em `0x170`, status em `0x177`, etc.).

### 2.3 Teclado (controlador 8042 / PS/2)

| Porta | Uso |
|-------|-----|
| `0x60` | Dados: leitura de scan code (após bit 0 de 0x64 setado) |
| `0x64` | Status: bit 0 = output buffer full, bit 1 = input buffer full; Command (write) |

Comandos úteis: `0xA8` (enable auxiliary/mouse), `0xD4` (próximo byte para o mouse).

### 2.4 VGA (modo texto e modo 13h)

**CRTC (indexado):** `0x3D4` (index), `0x3D5` (data).  
**Sequencer:** `0x3C4` (index), `0x3C5` (data).  
**Graphics:** `0x3CE` (index), `0x3CF` (data).  
**Attribute:** `0x3C0` (index/data, flip-flop via `0x3DA`).  
**Misc Output:** `0x3C2` (write).  
**Paleta (modo 13h):** `0x3C8` (índice), `0x3C9` (R, G, B).  
**Input Status:** `0x3DA` (read, reseta flip-flop do attribute).

Memória: texto em `0xB8000`, gráfico 13h em `0xA0000`.

---

## 3. Processador: IDT, vetores, stacks

- **GDT:** stage 2 e `kernel_entry` definem GDTs temporárias de base 0; o kernel usa os seletores `0x08` (código) e `0x10` (dados).
- **IDT:** 256 entradas; gates de interrupção (type 0x8E), selector `0x08`.
  - **0–31:** Exceções CPU (Double Fault em 8, etc.); stubs em `isr_stubs.asm`; uso de **fault_stack**.
  - **32–47:** IRQs 0–15 (PIC remapeado); stubs em `irq_stubs.asm`; uso de **irq_stack**; EOI em `irq.c`.
- **PIC:** Remapeado no boot para `0x20`/`0x28`; `pic_mask_all()` no início. O kernel mantém interrupções desligadas e faz polling; a IDT serve para exceções e IRQs mascaradas.
- **Stacks dedicadas:** `fault_stack` e `irq_stack` (4 KB cada, em `interrupt_stack.c`); funções `fault_stack_top()` e `irq_stack_top()` usadas pelo assembly.

---

## 4. Arquivos fonte

### 4.1 Assembly (`.asm`)

| Arquivo | Descrição |
|---------|-----------|
| **boot.asm** | Stage 1 (512 bytes). Real mode: lê `STAGE2_SECTORS` setores do LBA 1 para `0x7E00` via LBA estendido (ou CHS), grava `{boot_drive, kernel_lba}` em `0x4FF0` e salta para o stage 2. Não carrega o kernel nem entra em protected mode. |
| **stage2.asm** | Stage 2 em `0x7E00`: habilita A20, coleta E820 em `0x5200`, valida as faixas do kernel/app contra regiões utilizáveis, carrega o kernel em `0x01000000` por bounce buffer + cópia em modo protegido, carrega `app.bin` em `0x58000`, preenche `BootInfo` e chama `kernel_entry` com `EBX = 0x5000`. |
| **kernel_entry.asm** | Entrada 32-bit do kernel. Recebe `EBX = BootInfo*`, valida o magic, entra em long mode (identidade de 256 MiB + LFB) e chama `kmain(BootInfo*)` em cdecl; `esp` final `0x9FC00`, alinhado a 16. |
| **multiboot_header.asm** | Caminho legado para GRUB/Multiboot: monta um `BootInfo` estático em 16 MiB, coloca o ponteiro em `EBX` e chama `kernel_entry`. Sem mapa E820 (o `mm` usa fallback). |
| **isr_stubs.asm** | Stubs para exceções 0–31: macros `ISR_NOERR` (empilham err_code=0) e `ISR_ERR` (CPU já empilhou err_code); salvam registradores e segmentos; trocam para `fault_stack`; chamam `isr_dispatch(frame*)`; restauram pilha e fazem `iretq`. |
| **irq_stubs.asm** | Stubs para IRQ 0–15 (vetores 32–47): empilham err_code=0 e int_no=32+irq; trocam para `irq_stack`; chamam `isr_dispatch`; restauram e `iretq`. EOI é enviado em C em `irq_handle`. |

### 4.2 C – Núcleo e boot

| Arquivo | Descrição |
|---------|-----------|
| **kernel.c** | Núcleo principal: `kmain(const struct BootInfo*)` (valida BootInfo, zera BSS, `mm_init`, serial, depois idt/pic; sem `sti`), shell com histórico, comandos (help, echo, ls, clear, type, load, edit, mount, fdisk, format, copy, disks, free, df, dstat, visual, mouse, etc.). Driver ATA PIO: `disk_read_sector`, `disk_write_sector` (portas 0x1F0 / 0x170), validação de buffer, `cli` nas rotinas de disco. VGA texto: cursor, `putc`, `puts_k`. Teclado: leitura por polling em 0x64/0x60. Buffers fixos para fdisk em 0x80200 e 0x80600. |
| **bootinfo.h** / **bootinfo.c** | Contrato `BootInfo` v1 (128 bytes), `BootMemEntry` (24 bytes), magic/versão, validação e acesso global usado por `kmain`/`mm.c`. |
| **mm.c** / **mm.h** | First-fit em duas arenas. `kmalloc` em `0xA80000`–`0xC80000`; `umalloc` começa após `__bss_end`/`kernel_size` e usa o mapa E820 do BootInfo para escolher uma região utilizável até 256 MiB, sem sondar memória reservada. |
| **disk.h** | Protótipos de `disk_read_sector` e `disk_write_sector`; declaração das variáveis de debug ATA (`last_ata_*`). |

### 4.3 C – Interrupções e PIC

| Arquivo | Descrição |
|---------|-----------|
| **idt.c** / **idt.h** | IDT: 256 entradas, `set_gate()`, instalação de ISRs 0–31 e IRQs 32–47, `lidt`. |
| **isr.c** / **isr.h** | `isr_dispatch(isr_frame_t*)`: se int_no 32–47 chama `irq_handle` (EOI) e retorna; senão exceção: força modo texto VGA, imprime vetor, err_code, EIP, ESP, EBP, etc., e halt. `isr_df_panic` para double fault. Estrutura `isr_frame_t` com registradores e int_no/err_code. |
| **irq.c** / **irq.h** | `irq_handle(isr_frame_t*)`: para int_no 32–47 envia `pic_send_eoi(irq)`. |
| **pic.c** / **pic.h** | 8259: `pic_remap(offset1, offset2)`, `pic_mask_all()`, `pic_unmask(irq)`, `pic_send_eoi(irq)`. Portas 0x20/0x21 e 0xA0/0xA1. |
| **interrupt_stack.c** / **interrupt_stack.h** | Arrays `irq_stack` e `fault_stack` (4 KB, alinhados 16); funções `irq_stack_top()` e `fault_stack_top()` para o assembly. |

### 4.4 C – Disco, partições e FAT

| Arquivo | Descrição |
|---------|-----------|
| **partition.c** | MBR: `read_mbr(drive, mbr)` lê setor 0 para buffer em 0x80000, verifica assinatura 0xAA55. `write_mbr`, `create_partition`, `list_partitions`, `get_partition_info`. Buffers em 0x80000, 0x80200, 0x80400, 0x80600 conforme mapa de memória. |
| **fat.c** | FAT12/FAT16: buffers em 0xA00000 (boot sector), 0xA01000 (FAT), 0xA41000 (root dir) e 0xA51000 (diretório aberto). `fat_set_drive`, `fat_set_partition_offset`, `fat_mount`, `fat_read_file`, `fat_list_files`, `fat_write_file`, `fat_find_file`, `fat_get_space_info`. Usa `disk_read_sector` / `disk_write_sector`. |
| **fat_format.c** | Formatação FAT16: `format_fat16(drive, lba_start, total_sectors, volume_label)`. |

### 4.5 C – Interface e aplicações

| Arquivo | Descrição |
|---------|-----------|
| **visual.c** / **visual.h** | Modo gráfico VGA 13h (320×200, 256 cores): salvamento/restauração de estado VGA, modo texto 80×25. `visual_run()` desenha tela e espera tecla (com timeout). `vga_force_text_mode()` para panic em modo gráfico. Portas VGA e framebuffer 0xA0000. |
| **files.c** | Arquivos em memória (ex.: `exemplo.md`, `readme.txt`); `find_file`, tabela `files[]`. |
| **executables.c** | Executáveis embutidos (hello, testapp, editor): binários e entry points; tabela `executables[]`. Usado por `load` e `edit`. |

### 4.6 C – Utilitários e apps carregáveis

| Arquivo | Descrição |
|---------|-----------|
| **trace.c** / **trace.h** | Rastreamento leve: `trace_event(tag, a, b, c)`, `trace_dump_vga(row)`. Macro `TRACE_FUNC(tag)` para debug. |
| **app.c** | Aplicativo mínimo legado carregado pelo stage 2 em `0x58000`; recebe `puts_k` e pode imprimir. |
| **editor.c** | Editor de texto (carregado como executável): teclado via portas 0x64/0x60, salvar em memória/disco. |
| **hello.c** / **testapp.c** | Aplicativos de teste compilados separadamente e embutidos em `executables.c`. |

### 4.7 Linker e build

| Arquivo | Descrição |
|---------|-----------|
| **linker.ld** | Script de link: entrada `kernel_entry`, origem em `0x01000000`; seções `.text`, `.rodata`, `.data`, `.bss`; `format_buf_region` é símbolo absoluto em `0x80800` (não entra no `kernel.bin`). |
| **linker_multiboot.ld** | Variante legada para kernel Multiboot (GRUB), também em `0x01000000`. |
| **tools/validate_layout.sh** | Valida tamanhos, entry point, `__bss_end`, segmentos `LOAD`, símbolo absoluto do format buffer e sobreposições antes de gerar `os.img`. |
| **Makefile** | Compila kernel (`gcc -ffreestanding -m64`, `nasm -f elf64`), aplicativos 32 bits, stage 1, stage 2, `kernel.elf`/`kernel.bin` e `os.img`; mantém `run`, `run-log`, `debug`, `iso-grub`. |

---

## 5. Resumo de portas e endereços críticos

| Recurso | Endereço / Porta |
|---------|-------------------|
| BootInfo v1 | `0x5000` |
| Mapa E820 | `0x5200` |
| Stage 1 / Stage 2 | `0x7C00` / `0x7E00` |
| Kernel load/link | `0x01000000` |
| App load | `0x58000` |
| Stack (ESP inicial) | `0x9FC00` |
| VGA texto | `0xB8000` |
| VGA modo 13h | `0xA0000` |
| MBR / buffers fdisk | `0x80000`, `0x80200`, `0x80400`, `0x80600` |
| Buffer de formatação FAT | `0x80800` |
| FAT buffers | `0xA00000`, `0xA01000`, `0xA41000`, `0xA51000` |
| Heap do kernel | `0xA80000`–`0xC80000` |
| Heap alto (`umalloc`) | após `__bss_end`, até `0x0FFE0000` (E820) |
| PIC master | `0x20` (cmd), `0x21` (data) |
| PIC slave | `0xA0` (cmd), `0xA1` (data) |
| ATA primary | `0x1F0`–`0x1F7` |
| ATA secondary | `0x170`–`0x177` |
| Teclado / 8042 | `0x60` (data), `0x64` (status/cmd) |
| IRQs após remap | Vetores `0x20`–`0x2F` (IRQ0–15) |

Esta documentação reflete o estado do código com PIC remapeado, stacks separadas para falhas e IRQs, e um único `sti()` após a inicialização completa.

---

## 6. Depuração (debug linha a linha)

Sim. A forma prática é usar **QEMU + GDB**: o QEMU abre um servidor GDB e você conecta com `gdb`, carrega os símbolos do `kernel.elf` e depura com breakpoints e step.

### 6.1 Pré-requisitos

- `qemu-system-x86_64`
- `gdb` (pacote `gdb` no Linux; no Windows, MinGW GDB ou WSL)

### 6.2 Compilar com símbolos de debug

Para **step linha a linha** coerente, compile com otimização baixa e símbolos:

```bash
make kernel.elf CFLAGS="-ffreestanding -fno-pic -fno-stack-protector -fno-pie -nostdlib -O0 -g -m64 -mcmodel=small -mno-red-zone -march=x86-64 -mno-sse -mno-mmx -Werror -Wstrict-prototypes -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -I. -Ikernel -Ifs -Inet -Ivideo"
# (recompila só se alterar CFLAGS manualmente; o Makefile normal usa -O2)
```

Ou use o alvo que deixa o build em modo debug (veja Makefile, alvo `run-gdb` / build com `DEBUG=1`).

### 6.3 Subir o QEMU esperando o GDB

Em um terminal:

```bash
make run-gdb
```

Isso sobe o QEMU com:
- `-s` → servidor GDB na porta **1234**
- `-S` → CPU parada no início (só continua quando o GDB der `continue`)

A VM fica parada até você conectar e dar `c` (continue).

### 6.4 Conectar o GDB e carregar o kernel

Em **outro** terminal, no diretório do projeto:

```bash
gdb -ex "target remote :1234" -ex "symbol-file kernel.elf" -ex "break kmain" -ex "c"
```

Ou passo a passo dentro do GDB:

```
(gdb) target remote :1234
(gdb) symbol-file kernel.elf
(gdb) break kmain
(gdb) break disk_read_sector
(gdb) continue
```

Quando parar em um breakpoint:

- `n` (next)  — próxima linha (não entra em função)
- `s` (step)  — próxima linha (entra em função)
- `c` (continue) — continua até o próximo breakpoint
- `info registers` — registradores
- `p variável` — imprimir variável
- `x/8x 0x80000` — examinar memória (8 words em hex em 0x80000)

### 6.5 Ponto de entrada e endereços

O stage 2 salta para o **kernel_entry** (stub 32 bits), que entra em long mode e chama `kmain(BootInfo*)`. O código C do kernel está em endereços a partir de **0x01000000** (16 MiB). O `symbol-file kernel.elf` faz o GDB saber onde está cada função/linha.

Para parar já na primeira instrução do stub de entrada:

```
(gdb) break *0x01000000
(gdb) continue
```

(Substitua `0x01000000` pelo endereço real de `kernel_entry` se o linker mudar; `nm kernel.elf | grep kernel_entry`.)

### 6.6 Limitações

- **Interrupções desligadas:** Em trechos que rodam com `cli` (ex.: dentro de `disk_read_sector`), o timer não avança; o step continua normal.
- **Código otimizado (-O2):** O step pode “pular” linhas ou parecer estranho; use `-O0` para depuração linha a linha mais previsível.
- **Assembly:** Para os stubs (isr_stubs.asm, irq_stubs.asm), você pode usar `break *endereço` ou carregar o .elf e usar `break isr_dispatch` para parar ao entrar no handler C.
