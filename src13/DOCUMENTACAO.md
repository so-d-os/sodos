# Documentação do Sistema Operacional (kernel i386 freestanding)

Kernel 32-bit educacional para arquitetura i386, sem libc (freestanding), com shell, disco ATA, partições MBR, FAT12/FAT16, VGA texto/gráfico e tratamento de exceções/IRQs.

---

## 1. Mapa de memória (endereços físicos)

| Endereço        | Tamanho / Uso |
|-----------------|----------------|
| `0x00007C00`    | Boot sector (512 bytes); stack em real mode |
| `0x00010000`    | Início do kernel (`.text`, `.rodata`, `.data`, `.bss`) — definido em `linker.ld` |
| `0x00020000`    | Carga de aplicativos (boot carrega app aqui; Multiboot usa como `app_addr`) |
| `0x00030000`    | Executáveis carregados (load/run/editor) |
| `0x00040000+`   | Memória para `mem_files` e buffer do editor |
| `0x00050000`    | `filename_buffer` (cmd_edit) |
| `0x00060000`    | FAT: boot sector (512 bytes) |
| `0x00061000`    | FAT: tabela FAT em memória |
| `0x00062000`    | FAT: root directory |
| `0x00060000`–`0x000100000` | Faixa permitida para buffers de I/O de disco (driver rejeita fora disso) |
| `0x00080000`    | Buffer MBR (512 bytes) — leitura/escrita de partições |
| `0x00080200`    | Buffer de saída do `fdisk list` (512 bytes) |
| `0x00080400`    | Buffer de verificação do `write_mbr` (512 bytes) |
| `0x00080600`    | Buffer string do drive no `fdisk list` (32 bytes) |
| `0x00090000`–`0x000A0000` | **Pilha do kernel** (evitar buffers de disco aqui) |
| `0x0009FC00`    | Stack pointer inicial (boot.asm e multiboot: `esp = 0x9FC00` alinhado a 16) |
| `0x000A0000`    | Framebuffer VGA modo 13h (320×200, 256 cores) — 64 KB |
| `0x000B8000`    | Memória de texto VGA 80×25 (2 bytes por célula) |

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

- **GDT:** Boot define segmentos de código e dados (selector de código `0x08`, dados `0x10`).
- **IDT:** 256 entradas; gates de interrupção 32-bit (type 0x8E), selector `0x08`.
  - **0–31:** Exceções CPU (Double Fault em 8, etc.); stubs em `isr_stubs.asm`; uso de **fault_stack**.
  - **32–47:** IRQs 0–15 (PIC remapeado); stubs em `irq_stubs.asm`; uso de **irq_stack**; EOI em `irq.c`.
- **PIC:** Remapeado no boot para `0x20`/`0x28`; `pic_mask_all()` no início; depois `pic_unmask(0)` (timer) e `pic_unmask(1)` (teclado); `sti()` só após IDT + PIC + handlers.
- **Stacks dedicadas:** `fault_stack` e `irq_stack` (4 KB cada, em `interrupt_stack.c`); funções `fault_stack_top()` e `irq_stack_top()` usadas pelo assembly.

---

## 4. Arquivos fonte

### 4.1 Assembly (`.asm`)

| Arquivo | Descrição |
|---------|-----------|
| **boot.asm** | Stage 1 (512 bytes). Real mode, carrega kernel em `0x10000` e app em `0x20000` via LBA, habilita A20, configura GDT, entra em protected mode, `esp = 0x9FC00`, chama `kernel_entry` (jmp far para código 32-bit). |
| **kernel_entry.asm** | Entrada 32-bit do kernel. Recebe `ebx = app_addr`, `ecx = app_size`; coloca na pilha e chama `kmain(app_addr, app_size)` em cdecl; alinhamento 16 bytes em `esp`. |
| **multiboot_header.asm** | Cabeçalho Multiboot para GRUB; ponto de entrada `multiboot_entry`: configura `esp = 0x9FC00`, chama `kmain(0x20000, 0)` (sem app). |
| **isr_stubs.asm** | Stubs para exceções 0–31: macros `ISR_NOERR` (empilham err_code=0) e `ISR_ERR` (CPU já empilhou err_code); salvam registradores e segmentos; trocam para `fault_stack`; chamam `isr_dispatch(frame*)`; restauram pilha e fazem `iretd`. |
| **irq_stubs.asm** | Stubs para IRQ 0–15 (vetores 32–47): empilham err_code=0 e int_no=32+irq; trocam para `irq_stack`; chamam `isr_dispatch`; restauram e `iretd`. EOI é enviado em C em `irq_handle`. |

### 4.2 C – Núcleo e boot

| Arquivo | Descrição |
|---------|-----------|
| **kernel.c** | Núcleo principal: `kmain()` (cli, idt_init, pic_remap, pic_mask_all, pic_unmask 0/1, sti), shell com histórico, comandos (help, echo, ls, clear, type, load, edit, mount, fdisk, format, copy, disks, free, df, dstat, visual, mouse, etc.). Driver ATA PIO: `disk_read_sector`, `disk_write_sector` (portas 0x1F0 / 0x170), validação de buffer, `cli` nas rotinas de disco. VGA texto: cursor, `putc`, `puts_k`. Teclado: leitura por polling em 0x64/0x60. Buffers fixos para fdisk em 0x80200 e 0x80600. |
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
| **fat.c** | FAT12/FAT16: buffers em 0x60000 (boot sector), 0x61000 (FAT), 0x62000 (root dir). `fat_set_drive`, `fat_set_partition_offset`, `fat_mount`, `fat_read_file`, `fat_list_files`, `fat_write_file`, `fat_find_file`, `fat_get_space_info`. Usa `disk_read_sector` / `disk_write_sector`. |
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
| **app.c** | Aplicativo mínimo carregado pelo boot (ou Multiboot em 0x20000); recebe `puts_k` e pode imprimir. |
| **editor.c** | Editor de texto (carregado como executável): teclado via portas 0x64/0x60, salvar em memória/disco. |
| **hello.c** / **testapp.c** | Aplicativos de teste compilados separadamente e embutidos em `executables.c`. |

### 4.7 Linker e build

| Arquivo | Descrição |
|---------|-----------|
| **linker.ld** | Script de link: entrada `kmain`, origem em `0x10000`; seções `.text`, `.rodata`, `.data`, `.bss`. |
| **linker_multiboot.ld** | Variante para kernel Multiboot (GRUB). |
| **Makefile** | Compila kernel (gcc -ffreestanding -m32, nasm -f elf32 para .asm), gera kernel.elf/kernel.bin, os.img, e opcionalmente kernel_multiboot.elf e ISO. |

---

## 5. Resumo de portas e endereços críticos

| Recurso | Endereço / Porta |
|---------|-------------------|
| Kernel load | `0x10000` |
| App load | `0x20000` |
| Stack (ESP inicial) | `0x9FC00` |
| VGA texto | `0xB8000` |
| VGA modo 13h | `0xA0000` |
| MBR / buffers fdisk | `0x80000`, `0x80200`, `0x80400`, `0x80600` |
| FAT buffers | `0x60000`, `0x61000`, `0x62000` |
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

- `qemu-system-i386`
- `gdb` (pacote `gdb` no Linux; no Windows, MinGW GDB ou WSL)

### 6.2 Compilar com símbolos de debug

Para **step linha a linha** coerente, compile com otimização baixa e símbolos:

```bash
make kernel.elf CFLAGS="-ffreestanding -fno-pic -fno-stack-protector -fno-pie -nostdlib -O0 -g -m32 -march=i386 -Werror -Wstrict-prototypes"
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

O boot salta para o **kernel_entry** (assembly), que chama `kmain`. O código C do kernel está em endereços a partir de **0x10000**. O `symbol-file kernel.elf` faz o GDB saber onde está cada função/linha.

Para parar já na primeira instrução do kernel 32-bit:

```
(gdb) break *0x10000
(gdb) continue
```

(Substitua `0x10000` pelo endereço real de `kernel_entry` se o linker mudar; `readelf -s kernel.elf | grep kernel_entry`.)

### 6.6 Limitações

- **Interrupções desligadas:** Em trechos que rodam com `cli` (ex.: dentro de `disk_read_sector`), o timer não avança; o step continua normal.
- **Código otimizado (-O2):** O step pode “pular” linhas ou parecer estranho; use `-O0` para depuração linha a linha mais previsível.
- **Assembly:** Para os stubs (isr_stubs.asm, irq_stubs.asm), você pode usar `break *endereço` ou carregar o .elf e usar `break isr_dispatch` para parar ao entrar no handler C.
