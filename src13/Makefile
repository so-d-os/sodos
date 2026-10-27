
CC=gcc
LD=ld
AS=nasm

# Kernel em long mode (identidade, abaixo de 2 GB). Sem red zone: IRQ usa a pilha.
# -I. acha shell.h. kernel, fs, net e video guardam o nucleo.
CFLAGS=-ffreestanding -fno-pic -fno-stack-protector -fno-pie -nostdlib -O2 -fno-jump-tables \
	-m64 -mcmodel=small -mno-red-zone -march=x86-64 -mno-sse -mno-mmx \
	-Werror -Wstrict-prototypes -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
	-I. -Ikernel -Ifs -Inet -Ivideo
LDFLAGS=-m elf_x86_64 -z max-page-size=0x1000
# Hello/testapp/editor continuam 32 bits (so entram no disco; o kernel 64 nao os executa).
APP_CFLAGS=-ffreestanding -fno-pic -fno-stack-protector -fno-pie -nostdlib -O2 -fno-jump-tables \
	-m32 -march=i386 -mtune=i386 -mno-sse -mno-mmx -mno-3dnow \
	-Werror -Wstrict-prototypes -I.
APP_LDFLAGS=-m elf_i386
# 256 MB cobre o probe de mm.c (UM_PROBE_MAX) e as paginas de 2 MB do kernel_entry.
# -usb -device usb-tablet: o QEMU entrega a posicao absoluta do ponteiro.
QEMU=qemu-system-x86_64 -m 256 -usb -device usb-tablet

all: os.img

executables.c: hello.bin testapp.bin hello.elf testapp.elf

boot.bin: kernel/boot.asm kernel.elf kernel.bin
	@KERNEL_ENTRY=$$(readelf -h kernel.elf 2>/dev/null | grep -E "(Entry point|ponto de entrada)" | awk '{print $$NF}' | sed 's/0x//' | head -1); \
	if [ -z "$$KERNEL_ENTRY" ]; then KERNEL_ENTRY=1026c; fi; \
	KERNEL_SIZE=$$(wc -c < kernel.bin 2>/dev/null || echo 0); \
	if [ "$$KERNEL_SIZE" -lt 1 ]; then KERNEL_SIZE=36864; fi; \
	KERNEL_SECTORS=$$(( (KERNEL_SIZE + 511) / 512 )); \
	if [ "$$KERNEL_SECTORS" -lt 72 ]; then KERNEL_SECTORS=72; fi; \
	echo "Building boot.bin with KERNEL_ENTRY=0x$$KERNEL_ENTRY, KERNEL_SECTORS=$$KERNEL_SECTORS (kernel.bin=$$KERNEL_SIZE bytes)"; \
	$(AS) -f bin -DKERNEL_ENTRY=0x$$KERNEL_ENTRY -DKERNEL_SECTORS=$$KERNEL_SECTORS kernel/boot.asm -o $@



# Compila cabeçalho Multiboot
kernel/multiboot_header.o: kernel/multiboot_header.asm
	$(AS) -f elf32 kernel/multiboot_header.asm -o kernel/multiboot_header.o

# Stubs de exceção (IDT/ISR)
kernel/isr_stubs.o: kernel/isr_stubs.asm
	$(AS) -f elf64 kernel/isr_stubs.asm -o kernel/isr_stubs.o

# Stubs de IRQ (32..47)
kernel/irq_stubs.o: kernel/irq_stubs.asm
	$(AS) -f elf64 kernel/irq_stubs.asm -o kernel/irq_stubs.o

# Comandos do shell (pasta shell/)
SHELL_CMD_C = shell/cmd_help.c shell/cmd_echo.c shell/cmd_ls.c shell/cmd_clear.c shell/cmd_cls.c shell/cmd_history.c shell/cmd_type.c shell/cmd_pause.c shell/cmd_load.c shell/cmd_edit.c shell/cmd_asm.c shell/cmd_free.c shell/cmd_visual.c shell/cmd_mouse.c shell/cmd_win.c shell/cmd_ping.c shell/cmd_dlls.c
SHELL_CMD_O = $(SHELL_CMD_C:.c=.o)

# Comandos de discos (pasta disks/)
DISKS_CMD_C = disks/cmd_mount.c disks/cmd_dir.c disks/cmd_cat.c disks/cmd_fdisk.c disks/cmd_format.c disks/cmd_copy.c disks/cmd_disks.c disks/cmd_df.c disks/cmd_dstat.c
DISKS_CMD_O = $(DISKS_CMD_C:.c=.o)

CMD_O = $(SHELL_CMD_O) $(DISKS_CMD_O)

kernel.elf: kernel/kernel.c fs/files.c executables.c fs/fat.c fs/partition.c fs/fat_format.c video/visual.c video/font8x8_data.c kernel/serial.c kernel/idt.c kernel/isr.c kernel/pic.c kernel/irq.c kernel/interrupt_stack.c kernel/trace.c fs/drives.c kernel/mm.c net/net.c net/httpd.c kernel/module.c kernel/sched.c kernel/kexports.c $(SHELL_CMD_C) $(DISKS_CMD_C) shell/cmd_shutdown.asm kernel/isr_stubs.asm kernel/irq_stubs.asm kernel/modrun.asm kernel/kernel_entry.asm $(wildcard *.h kernel/*.h fs/*.h net/*.h video/*.h) c_compiler/sox.h
	$(CC) $(CFLAGS) -c kernel/kernel.c -o kernel/kernel.o
	$(CC) $(CFLAGS) -c kernel/mm.c -o kernel/mm.o
	$(CC) $(CFLAGS) -c net/net.c -o net/net.o
	$(CC) $(CFLAGS) -c kernel/sched.c -o kernel/sched.o
	$(CC) $(CFLAGS) -c net/httpd.c -o net/httpd.o
	$(CC) $(CFLAGS) -c kernel/module.c -o kernel/module.o
	$(CC) $(CFLAGS) -c kernel/kexports.c -o kernel/kexports.o
	$(CC) $(CFLAGS) -c fs/files.c -o fs/files.o
	$(CC) $(CFLAGS) -c executables.c -o executables.o
	$(CC) $(CFLAGS) -c fs/fat.c -o fs/fat.o
	$(CC) $(CFLAGS) -c fs/drives.c -o fs/drives.o
	$(CC) $(CFLAGS) -c fs/partition.c -o fs/partition.o
	$(CC) $(CFLAGS) -c fs/fat_format.c -o fs/fat_format.o
	$(CC) $(CFLAGS) -c video/visual.c -o video/visual.o
	$(CC) $(CFLAGS) -c video/font8x8_data.c -o video/font8x8_data.o
	$(CC) $(CFLAGS) -c kernel/serial.c -o kernel/serial.o
	$(CC) $(CFLAGS) -c kernel/idt.c -o kernel/idt.o
	$(CC) $(CFLAGS) -c kernel/isr.c -o kernel/isr.o
	$(CC) $(CFLAGS) -c kernel/pic.c -o kernel/pic.o
	$(CC) $(CFLAGS) -c kernel/irq.c -o kernel/irq.o
	$(CC) $(CFLAGS) -c kernel/interrupt_stack.c -o kernel/interrupt_stack.o
	$(CC) $(CFLAGS) -c kernel/trace.c -o kernel/trace.o
	$(CC) $(CFLAGS) -c shell/cmd_help.c -o shell/cmd_help.o
	$(CC) $(CFLAGS) -c shell/cmd_echo.c -o shell/cmd_echo.o
	$(CC) $(CFLAGS) -c shell/cmd_ls.c -o shell/cmd_ls.o
	$(CC) $(CFLAGS) -c shell/cmd_clear.c -o shell/cmd_clear.o
	$(CC) $(CFLAGS) -c shell/cmd_cls.c -o shell/cmd_cls.o
	$(CC) $(CFLAGS) -c shell/cmd_history.c -o shell/cmd_history.o
	$(CC) $(CFLAGS) -c shell/cmd_type.c -o shell/cmd_type.o
	$(CC) $(CFLAGS) -c shell/cmd_pause.c -o shell/cmd_pause.o
	$(CC) $(CFLAGS) -c shell/cmd_load.c -o shell/cmd_load.o
	$(CC) $(CFLAGS) -c shell/cmd_edit.c -o shell/cmd_edit.o
	$(CC) $(CFLAGS) -c shell/cmd_asm.c -o shell/cmd_asm.o
	$(CC) $(CFLAGS) -c shell/cmd_free.c -o shell/cmd_free.o
	$(CC) $(CFLAGS) -c shell/cmd_visual.c -o shell/cmd_visual.o
	$(CC) $(CFLAGS) -c shell/cmd_mouse.c -o shell/cmd_mouse.o
	$(CC) $(CFLAGS) -c shell/cmd_win.c -o shell/cmd_win.o
	$(CC) $(CFLAGS) -c shell/cmd_ping.c -o shell/cmd_ping.o
	$(CC) $(CFLAGS) -c shell/cmd_dlls.c -o shell/cmd_dlls.o
	$(AS) -f elf64 shell/cmd_shutdown.asm -o shell/cmd_shutdown.o
	$(CC) $(CFLAGS) -c disks/cmd_mount.c -o disks/cmd_mount.o
	$(CC) $(CFLAGS) -c disks/cmd_dir.c -o disks/cmd_dir.o
	$(CC) $(CFLAGS) -c disks/cmd_cat.c -o disks/cmd_cat.o
	$(CC) $(CFLAGS) -c disks/cmd_fdisk.c -o disks/cmd_fdisk.o
	$(CC) $(CFLAGS) -c disks/cmd_format.c -o disks/cmd_format.o
	$(CC) $(CFLAGS) -c disks/cmd_copy.c -o disks/cmd_copy.o
	$(CC) $(CFLAGS) -c disks/cmd_disks.c -o disks/cmd_disks.o
	$(CC) $(CFLAGS) -c disks/cmd_df.c -o disks/cmd_df.o
	$(CC) $(CFLAGS) -c disks/cmd_dstat.c -o disks/cmd_dstat.o
	$(AS) -f elf64 kernel/isr_stubs.asm -o kernel/isr_stubs.o
	$(AS) -f elf64 kernel/irq_stubs.asm -o kernel/irq_stubs.o
	$(AS) -f elf64 kernel/modrun.asm -o kernel/modrun.o
	$(AS) -f elf64 kernel/kernel_entry.asm -o kernel/kernel_entry.o
	$(LD) $(LDFLAGS) -T kernel/linker.ld -o $@ kernel/kernel_entry.o kernel/kernel.o fs/files.o executables.o fs/fat.o fs/partition.o fs/fat_format.o video/visual.o video/font8x8_data.o kernel/serial.o kernel/idt.o kernel/isr.o kernel/pic.o kernel/irq.o kernel/interrupt_stack.o kernel/trace.o fs/drives.o kernel/mm.o net/net.o net/httpd.o kernel/module.o kernel/sched.o kernel/kexports.o $(CMD_O) shell/cmd_shutdown.o kernel/isr_stubs.o kernel/irq_stubs.o kernel/modrun.o

# Kernel com suporte Multiboot para GRUB
kernel_multiboot.elf: kernel/multiboot_header.o kernel/kernel.c fs/files.c executables.c fs/fat.c fs/partition.c fs/fat_format.c video/visual.c video/font8x8_data.c kernel/serial.c kernel/idt.c kernel/isr.c kernel/pic.c kernel/irq.c kernel/interrupt_stack.c kernel/trace.c kernel/isr_stubs.asm kernel/irq_stubs.asm
	$(CC) $(CFLAGS) -c kernel/kernel.c -o kernel/kernel.o
	$(CC) $(CFLAGS) -c fs/files.c -o fs/files.o
	$(CC) $(CFLAGS) -c net/net.c -o net/net.o
	$(CC) $(CFLAGS) -c executables.c -o executables.o
	$(CC) $(CFLAGS) -c fs/fat.c -o fs/fat.o
	$(CC) $(CFLAGS) -c fs/partition.c -o fs/partition.o
	$(CC) $(CFLAGS) -c fs/fat_format.c -o fs/fat_format.o
	$(CC) $(CFLAGS) -c video/visual.c -o video/visual.o
	$(CC) $(CFLAGS) -c video/font8x8_data.c -o video/font8x8_data.o
	$(CC) $(CFLAGS) -c kernel/serial.c -o kernel/serial.o
	$(CC) $(CFLAGS) -c kernel/idt.c -o kernel/idt.o
	$(CC) $(CFLAGS) -c kernel/isr.c -o kernel/isr.o
	$(CC) $(CFLAGS) -c kernel/pic.c -o kernel/pic.o
	$(CC) $(CFLAGS) -c kernel/irq.c -o kernel/irq.o
	$(CC) $(CFLAGS) -c kernel/interrupt_stack.c -o kernel/interrupt_stack.o
	$(CC) $(CFLAGS) -c kernel/trace.c -o kernel/trace.o
	$(AS) -f elf64 kernel/isr_stubs.asm -o kernel/isr_stubs.o
	$(AS) -f elf64 kernel/irq_stubs.asm -o kernel/irq_stubs.o
	$(LD) $(LDFLAGS) -T kernel/linker_multiboot.ld -o $@ kernel/multiboot_header.o kernel/kernel.o fs/files.o executables.o fs/fat.o fs/partition.o fs/fat_format.o video/visual.o video/font8x8_data.o kernel/serial.o kernel/idt.o kernel/isr.o kernel/pic.o kernel/irq.o kernel/interrupt_stack.o kernel/trace.o net/net.o kernel/isr_stubs.o kernel/irq_stubs.o


kernel.bin: kernel.elf
	objcopy -O binary $< $@
	@BSS_END=$$(nm kernel.elf | awk '$$3=="__bss_end"{print $$1}'); \
	if [ -n "$$BSS_END" ] && [ "$$(printf '%d' 0x$$BSS_END)" -ge "$$(printf '%d' 0x58000)" ]; then \
	  echo "Erro: __bss_end=0x$$BSS_END ultrapassa 0x58000 (slot do asm/APP_LOAD)."; \
	  rm -f $@; \
	  exit 1; \
	fi


app.elf: app.c
	$(CC) $(APP_CFLAGS) -c app.c -o app.o
	$(LD) $(APP_LDFLAGS) -T app.ld -o $@ app.o

app.bin: app.elf
	objcopy -O binary $< $@

hello.elf: hello.c
	$(CC) $(APP_CFLAGS) -c hello.c -o hello.o
	$(LD) $(APP_LDFLAGS) -T hello.ld -o $@ hello.o

hello.bin: hello.elf
	objcopy -O binary $< $@

testapp.elf: testapp.c
	$(CC) $(APP_CFLAGS) -c testapp.c -o testapp.o
	$(LD) $(APP_LDFLAGS) -T testapp.ld -o $@ testapp.o

testapp.bin: testapp.elf
	objcopy -O binary $< $@

editor.elf: editor.c
	$(CC) $(APP_CFLAGS) -c editor.c -o editor.o
	$(LD) $(APP_LDFLAGS) -T editor.ld -o $@ editor.o

editor.bin: editor.elf
	objcopy -O binary $< $@

executables.c: hello.bin testapp.bin editor.bin hello.elf testapp.elf editor.elf
	@echo "// Executáveis embutidos - gerado automaticamente" > $@
	@echo "#include <stdint.h>" >> $@
	@echo "" >> $@
	@echo "struct executable_entry {" >> $@
	@echo "  const char* name;" >> $@
	@echo "  const uint8_t* data;" >> $@
	@echo "  uint32_t size;" >> $@
	@echo "  uint32_t entry_point;" >> $@
	@echo "};" >> $@
	@echo "" >> $@
	@echo "// hello.bin" >> $@
	@HELLO_SIZE=$$(wc -c < hello.bin); \
	echo "static const uint8_t hello_bin[] = {" >> $@; \
	od -A n -t x1 hello.bin | grep -v '^\*' | sed 's/\([0-9a-f][0-9a-f]\)/0x\1,/g' | sed 's/^/  /' | sed '$$s/,$$//' >> $@; \
	echo "" >> $@; \
	echo "};" >> $@; \
	echo "static const uint32_t hello_bin_len = $$HELLO_SIZE;" >> $@
	@echo "" >> $@
	@echo "// testapp.bin" >> $@
	@TESTAPP_SIZE=$$(wc -c < testapp.bin); \
	echo "static const uint8_t testapp_bin[] = {" >> $@; \
	od -A n -t x1 testapp.bin | grep -v '^\*' | sed 's/\([0-9a-f][0-9a-f]\)/0x\1,/g' | sed 's/^/  /' | sed '$$s/,$$//' >> $@; \
	echo "" >> $@; \
	echo "};" >> $@; \
	echo "static const uint32_t testapp_bin_len = $$TESTAPP_SIZE;" >> $@
	@echo "" >> $@
	@echo "// editor.bin" >> $@
	@EDITOR_SIZE=$$(wc -c < editor.bin); \
	echo "static const uint8_t editor_bin[] = {" >> $@; \
	od -A n -t x1 editor.bin | grep -v '^\*' | sed 's/\([0-9a-f][0-9a-f]\)/0x\1,/g' | sed 's/^/  /' | sed '$$s/,$$//' >> $@; \
	echo "" >> $@; \
	echo "};" >> $@; \
	echo "static const uint32_t editor_bin_len = $$EDITOR_SIZE;" >> $@
	@echo "" >> $@
	@echo "// Entry points (obtidos dos ELFs)" >> $@
	@HELLO_ENTRY=$$(readelf -h hello.elf 2>/dev/null | grep -E "(Entry point|ponto de entrada)" | awk '{print $$NF}' | sed 's/0x//' | head -1); \
	if [ -z "$$HELLO_ENTRY" ]; then HELLO_ENTRY=30000; fi; \
	TESTAPP_ENTRY=$$(readelf -h testapp.elf 2>/dev/null | grep -E "(Entry point|ponto de entrada)" | awk '{print $$NF}' | sed 's/0x//' | head -1); \
	if [ -z "$$TESTAPP_ENTRY" ]; then TESTAPP_ENTRY=30000; fi; \
	EDITOR_ENTRY=$$(readelf -h editor.elf 2>/dev/null | grep -E "(Entry point|ponto de entrada)" | awk '{print $$NF}' | sed 's/0x//' | head -1); \
	if [ -z "$$EDITOR_ENTRY" ]; then EDITOR_ENTRY=30000; fi; \
	echo "static const uint32_t hello_entry = 0x$$HELLO_ENTRY;" >> $@; \
	echo "static const uint32_t testapp_entry = 0x$$TESTAPP_ENTRY;" >> $@; \
	echo "static const uint32_t editor_entry = 0x$$EDITOR_ENTRY;" >> $@
	@echo "" >> $@
	@echo "const struct executable_entry executables[] = {" >> $@
	@echo "  {\"hello\", hello_bin, hello_bin_len, hello_entry}," >> $@
	@echo "  {\"testapp\", testapp_bin, testapp_bin_len, testapp_entry}," >> $@
	@echo "  {\"editor\", editor_bin, editor_bin_len, editor_entry}," >> $@
	@echo "};" >> $@
	@echo "" >> $@
	@echo "const int executable_count = 3;" >> $@

os.img: boot.bin kernel.bin app.bin
	@KERNEL_SIZE=$$(wc -c < kernel.bin); \
	KERNEL_SECTORS=$$(( (KERNEL_SIZE + 511) / 512 )); \
	if [ "$$KERNEL_SECTORS" -lt 72 ]; then KERNEL_SECTORS=72; fi; \
	APP_SEEK=$$((1 + KERNEL_SECTORS)); \
	TOTAL=$$((APP_SEEK + 8)); \
	if [ "$$TOTAL" -lt 128 ]; then TOTAL=128; fi; \
	echo "os.img: kernel_sectors=$$KERNEL_SECTORS app_seek=$$APP_SEEK total=$$TOTAL"; \
	dd if=/dev/zero of=os.img bs=512 count=$$TOTAL; \
	dd if=boot.bin   of=os.img bs=512 seek=0 conv=notrunc; \
	dd if=kernel.bin of=os.img bs=512 seek=1 conv=notrunc; \
	dd if=app.bin    of=os.img bs=512 seek=$$APP_SEEK conv=notrunc

# Gera uma imagem ISO bootável a partir do os.img
# Requer genisoimage ou mkisofs instalado no sistema
iso: os.img
	mkdir -p iso_root
	cp os.img iso_root/os.img
	@if command -v genisoimage >/dev/null 2>&1; then \
	  genisoimage -R -o so.iso \
	    -b os.img -no-emul-boot -boot-load-size 4 -boot-info-table \
	    iso_root; \
	elif command -v mkisofs >/dev/null 2>&1; then \
	  mkisofs -R -o so.iso \
	    -b os.img -no-emul-boot -boot-load-size 4 -boot-info-table \
	    iso_root; \
	else \
	  echo \"Erro: nem genisoimage nem mkisofs encontrados. Instale com:\"; \
	  echo \"  sudo apt-get install genisoimage\"; \
	  exit 1; \
	fi

# Disco secundário 256MB (drive 1). Criado no primeiro run se não existir.
# 256MB cabe o content/ (videos grandes) em D:\user\default\desktop.
disk.img: os.img
	@if [ ! -f disk.img ]; then \
	  echo "Creating 256MB secondary disk (disk.img)..."; \
	  dd if=/dev/zero of=disk.img bs=1M count=256 2>/dev/null || \
	  dd if=/dev/zero of=disk.img bs=512 count=524288 2>/dev/null || \
	  (echo "Error: Could not create disk.img"; exit 1); \
	fi

# Segundo disco secundário 100MB (drive 2). Criado no primeiro run se não existir.
disk2.img: os.img
	@if [ ! -f disk2.img ]; then \
	  echo "Creating 100MB secondary disk (disk2.img)..."; \
	  dd if=/dev/zero of=disk2.img bs=1M count=100 2>/dev/null || \
	  dd if=/dev/zero of=disk2.img bs=512 count=204800 2>/dev/null || \
	  (echo "Error: Could not create disk2.img"; exit 1); \
	fi

# run-log: usar sempre $(CURDIR) (pasta deste Makefile). Nao usar "cd ." com
# RUN_LOG_DIR — se RUN_LOG_DIR != pasta do os.img, o QEMU abria outro ficheiro
# ou o -serial file:./... apontava para o sitio errado e o log ficava vazio.

run: os.img disk.img disk2.img
	$(QEMU) -vga std -netdev user,id=n0,hostfwd=tcp::8797-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
	  -drive format=raw,file=os.img,if=ide,index=0 \
	  -drive format=raw,file=disk.img,if=ide,index=1 \
	  -drive format=raw,file=disk2.img,if=ide,index=2

# Formata disk.img (256MB) como FAT16 pelo Linux: MBR + partição 0 + mkfs.fat
# Uso: make format-disk [DISK=disk.img] [FAT_LABEL=MYDISK]
# Cluster de 8KB: 256MB continua FAT16 (o kernel recusa FAT32).
# Requer: dosfstools (mkfs.fat ou mkfs.vfat), util-linux (sfdisk)
DISK ?= disk.img
FAT_LABEL ?= MYDISK
# Incluir /usr/sbin e /sbin na busca (mkfs.fat/sfdisk costumam estar lá)
PATH_SBIN := /usr/sbin:/sbin:$(PATH)
SFDISK := $(shell PATH="$(PATH_SBIN)" command -v sfdisk 2>/dev/null || echo /usr/sbin/sfdisk)
MKFSFAT := $(shell PATH="$(PATH_SBIN)" command -v mkfs.fat 2>/dev/null || PATH="$(PATH_SBIN)" command -v mkfs.vfat 2>/dev/null || echo "")
format-disk:
	@echo "Formatando $(DISK) como FAT16 (rótulo $(FAT_LABEL))..."
	@if [ ! -f $(DISK) ]; then \
	  echo "Criando $(DISK) 256MB..."; \
	  dd if=/dev/zero of=$(DISK) bs=1M count=256 2>/dev/null || \
	  dd if=/dev/zero of=$(DISK) bs=512 count=524288 2>/dev/null; \
	fi
	@if [ ! -f $(DISK) ] || [ "$$(wc -c < $(DISK))" -lt 268435456 ]; then \
	  dd if=/dev/zero of=$(DISK) bs=1M seek=256 count=0 2>/dev/null || \
	  dd if=/dev/zero of=$(DISK) bs=1M count=256 conv=notrunc 2>/dev/null; \
	fi
	@echo "Criando MBR e partição (LBA 1, 524287 setores, tipo FAT16)..."
	@printf 'label: dos\nunit: sectors\nstart=1, size=524287, type=6\n' | $(SFDISK) $(DISK) --no-reread --force 2>/dev/null || \
	  (echo "Erro: sfdisk falhou. Tente: PATH=\"/usr/sbin:$$PATH\" make format-disk"; exit 1)
	@echo "Formatando partição com mkfs.fat -F 16..."
	@dd if=$(DISK) bs=512 skip=1 count=524287 of=$(DISK).part 2>/dev/null
	@if [ -z "$(MKFSFAT)" ]; then \
	  echo "Erro: mkfs.fat não encontrado. Instale dosfstools: sudo apt install dosfstools"; rm -f $(DISK).part; exit 1; \
	fi
	@$(MKFSFAT) -F 16 -s 16 -n $(FAT_LABEL) $(DISK).part || \
	  (echo "Erro: formatação falhou (veja acima). Se faltar o comando: sudo apt install dosfstools"; rm -f $(DISK).part; exit 1)
	@dd if=$(DISK).part of=$(DISK) bs=512 seek=1 conv=notrunc 2>/dev/null
	@rm -f $(DISK).part
	@echo "Pronto. $(DISK) está com MBR + partição FAT16. No SO: mount 0"

# Copia compilador, linker, libc, headers e exemplos para a raiz do disk.img
# (D: no SO). Formata antes se o disco ainda nao tiver FAT. Requer mtools.
# Nomes em maiusculo 8.3 para o mtools nao criar entradas de nome longo.
# Tudo vai para a raiz do D: em maiusculas: binarios, headers, fontes da libc,
# exemplos e os fontes do proprio cc/link (para recompilar dentro do SO).
DISK_FILES = c_compiler/out/*.EXE c_compiler/out/*.DLL c_compiler/out/*.OBJ \
	c_compiler/out/*.IMP c_compiler/include/*.h c_compiler/lib/*.c \
	c_compiler/examples/*.c c_compiler/examples/*.h c_compiler/examples/*.cpp \
	c_compiler/cc/*.c c_compiler/cc/cc.h c_compiler/sox.h c_compiler/link/link.c \
	c_compiler/tls/*.c c_compiler/tls/*.h c_compiler/gui/*.c \
	c_compiler/img/*.c c_compiler/browser/*.c c_compiler/browser/*.cpp c_compiler/browser/*.h
# browser/main.c vai como BROWSER.C para nao colidir com o MAIN.C do cc
disk-files: disk.img
	@if pgrep -f "[q]emu-system.*file=$(DISK)" >/dev/null; then \
	  echo "Feche o QEMU antes: o SO regrava a FAT e apaga o que o mcopy gravou."; exit 1; fi
	$(MAKE) -C c_compiler all
	@if ! mdir -i $(DISK)@@512 ::/ >/dev/null 2>&1; then $(MAKE) format-disk; fi
	@for f in $$(ls $(DISK_FILES) | grep -v '/TEST.EXE$$'); do \
	  case "$$f" in */browser/main.c) n=BROWSER.C ;; \
	  *) n=$$(basename "$$f" | tr 'a-z' 'A-Z') ;; esac; \
	  mcopy -o -i $(DISK)@@512 "$$f" "::/$$n" || exit 1; \
	done
	@mkdir -p c_compiler/build/desk
	@gcc -m32 -O2 -D__export= -Ic_compiler/include -o c_compiler/build/mkthumb \
	  c_compiler/img/img.c utils/mkthumb.c
	@if ! mdir -i $(DISK)@@512 ::/USER >/dev/null 2>&1; then mmd -i $(DISK)@@512 ::/USER; fi
	@if ! mdir -i $(DISK)@@512 ::/USER/DEFAULT >/dev/null 2>&1; then mmd -i $(DISK)@@512 ::/USER/DEFAULT; fi
	@if ! mdir -i $(DISK)@@512 ::/USER/DEFAULT/DESKTOP >/dev/null 2>&1; then mmd -i $(DISK)@@512 ::/USER/DEFAULT/DESKTOP; fi
	@for f in content/*; do \
	  [ -e "$$f" ] || continue; \
	  n=$$(basename "$$f" | tr 'a-z' 'A-Z'); \
	  if [ -d "$$f" ]; then \
	    if ! mdir -i $(DISK)@@512 "::/USER/DEFAULT/DESKTOP/$$n" >/dev/null 2>&1; then mmd -i $(DISK)@@512 "::/USER/DEFAULT/DESKTOP/$$n"; fi; \
	    for g in "$$f"/*; do \
	      [ -f "$$g" ] || continue; \
	      gn=$$(basename "$$g" | tr 'a-z' 'A-Z'); \
	      mcopy -o -i $(DISK)@@512 "$$g" "::/USER/DEFAULT/DESKTOP/$$n/$$gn" || exit 1; \
	    done; \
	  elif [ -f "$$f" ]; then \
	    mcopy -o -i $(DISK)@@512 "$$f" "::/USER/DEFAULT/DESKTOP/$$n" || exit 1; \
	    case "$$n" in \
	      *.PNG|*.JPG|*.JPE|*.GIF|*.ICO|*.BMP) \
	        b=$${n%.*}; \
	        ./c_compiler/build/mkthumb "$$f" "c_compiler/build/desk/$$b.THM" || exit 1; \
	        mcopy -o -i $(DISK)@@512 "c_compiler/build/desk/$$b.THM" "::/USER/DEFAULT/DESKTOP/$$b.THM" || exit 1; \
	        ;; \
	    esac; \
	  fi; \
	done
	@mcopy -o -i $(DISK)@@512 utils/image_view.c ::/IMAGEVW.C
	@if ! mdir -i $(DISK)@@512 ::/SODOS >/dev/null 2>&1; then mmd -i $(DISK)@@512 ::/SODOS; fi
	@if ! mdir -i $(DISK)@@512 ::/SODOS/FONTS >/dev/null 2>&1; then mmd -i $(DISK)@@512 ::/SODOS/FONTS; fi
	@mcopy -o -i $(DISK)@@512 c_compiler/out/FONTADV.EXE ::/SODOS/FONTADV.EXE
	@for f in fonts/ship/*; do \
	  [ -f "$$f" ] || continue; \
	  n=$$(basename "$$f" | tr 'a-z' 'A-Z'); \
	  mcopy -o -i $(DISK)@@512 "$$f" "::/SODOS/FONTS/$$n" || exit 1; \
	done
	@mdir -i $(DISK)@@512 -w ::/

# Sobe o QEMU e mostra os comandos para formatar o disco (digite no prompt do QEMU)
run-format: os.img disk.img disk2.img
	@echo ""
	@echo "=== Comandos para formatar (copie e cole no QEMU quando aparecer o prompt >) ==="
	@echo "fdisk create 0 1 524287 0x06 1"
	@echo "format 0 MYDISK"
	@echo "mount 0"
	@echo "dir"
	@echo "=== Fim dos comandos ==="
	@echo ""
	$(QEMU) -vga std -netdev user,id=n0,hostfwd=tcp::8797-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
	  -drive format=raw,file=os.img,if=ide,index=0 \
	  -drive format=raw,file=disk.img,if=ide,index=1 \
	  -drive format=raw,file=disk2.img,if=ide,index=2

# Build com símbolos e -O0 para debug linha a linha (use antes de run-gdb).
CFLAGS_DEBUG=-ffreestanding -fno-pic -fno-stack-protector -fno-pie -nostdlib -O0 -g \
	-m64 -mcmodel=small -mno-red-zone -march=x86-64 -mno-sse -mno-mmx -Werror -Wstrict-prototypes \
	-I. -Ikernel -Ifs -Inet -Ivideo
debug: kernel/kernel.c fs/files.c executables.c fs/fat.c fs/partition.c fs/fat_format.c video/visual.c video/font8x8_data.c kernel/serial.c kernel/idt.c kernel/isr.c kernel/pic.c kernel/irq.c kernel/interrupt_stack.c kernel/trace.c
	$(CC) $(CFLAGS_DEBUG) -c kernel/kernel.c -o kernel/kernel.o
	$(CC) $(CFLAGS_DEBUG) -c fs/files.c -o fs/files.o
	$(CC) $(CFLAGS_DEBUG) -c executables.c -o executables.o
	$(CC) $(CFLAGS_DEBUG) -c fs/fat.c -o fs/fat.o
	$(CC) $(CFLAGS_DEBUG) -c fs/partition.c -o fs/partition.o
	$(CC) $(CFLAGS_DEBUG) -c fs/fat_format.c -o fs/fat_format.o
	$(CC) $(CFLAGS_DEBUG) -c video/visual.c -o video/visual.o
	$(CC) $(CFLAGS_DEBUG) -c video/font8x8_data.c -o video/font8x8_data.o
	$(CC) $(CFLAGS_DEBUG) -c kernel/serial.c -o kernel/serial.o
	$(CC) $(CFLAGS_DEBUG) -c kernel/idt.c -o kernel/idt.o
	$(CC) $(CFLAGS_DEBUG) -c kernel/isr.c -o kernel/isr.o
	$(CC) $(CFLAGS_DEBUG) -c kernel/pic.c -o kernel/pic.o
	$(CC) $(CFLAGS_DEBUG) -c kernel/irq.c -o kernel/irq.o
	$(CC) $(CFLAGS_DEBUG) -c kernel/interrupt_stack.c -o kernel/interrupt_stack.o
	$(CC) $(CFLAGS_DEBUG) -c kernel/trace.c -o kernel/trace.o
	$(CC) $(CFLAGS_DEBUG) -c net/net.c -o net/net.o
	$(AS) -f elf64 kernel/isr_stubs.asm -o kernel/isr_stubs.o
	$(AS) -f elf64 kernel/irq_stubs.asm -o kernel/irq_stubs.o
	$(AS) -f elf64 kernel/kernel_entry.asm -o kernel/kernel_entry.o
	$(LD) $(LDFLAGS) -T kernel/linker.ld -o kernel.elf kernel/kernel_entry.o kernel/kernel.o fs/files.o executables.o fs/fat.o fs/partition.o fs/fat_format.o video/visual.o video/font8x8_data.o kernel/serial.o kernel/idt.o kernel/isr.o kernel/pic.o kernel/irq.o kernel/interrupt_stack.o kernel/trace.o net/net.o kernel/isr_stubs.o kernel/irq_stubs.o
	$(MAKE) boot.bin kernel.bin
	$(MAKE) os.img

# Debug: QEMU espera GDB na porta 1234 (-s) e inicia com CPU parada (-S).
# No outro terminal: gdb -ex "target remote :1234" -ex "symbol-file kernel.elf" -ex "break kmain" -ex "c"
run-gdb: os.img disk.img disk2.img
	$(QEMU) -s -S -vga std -netdev user,id=n0,hostfwd=tcp::8797-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
	  -drive format=raw,file=os.img,if=ide,index=0 \
	  -drive format=raw,file=disk.img,if=ide,index=1 \
	  -drive format=raw,file=disk2.img,if=ide,index=2

# Log do kernel em COM1 -> ficheiros na pasta do Makefile (caminhos absolutos).
# Apaga logs anteriores para cada sessao; fecha o QEMU para o ficheiro ser descarregado.
run-log: os.img disk.img disk2.img
	@echo "COM1 serial -> $(CURDIR)/qemu_serial.log"
	@echo "QEMU trace  -> $(CURDIR)/qemu.log"
	@rm -f "$(CURDIR)/qemu_serial.log" "$(CURDIR)/qemu.log"
	$(QEMU) -vga std -netdev user,id=n0,hostfwd=tcp::8797-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
	  -drive format=raw,file=$(CURDIR)/os.img,if=ide,index=0 \
	  -drive format=raw,file=$(CURDIR)/disk.img,if=ide,index=1 \
	  -drive format=raw,file=$(CURDIR)/disk2.img,if=ide,index=2 \
	  -D "$(CURDIR)/qemu.log" -d cpu_reset,guest_errors,int \
	  -no-reboot -no-shutdown \
	  -serial "file:$(CURDIR)/qemu_serial.log"

# COM1 para stdout + copia para qemu_serial.log (tee). Sem stdbuf, stdout para
# pipe/tee fica em buffer completo (~4KiB) e as linhas [visual] so aparecem ao
# fechar o QEMU — stdbuf -oL forca uma linha de cada vez (coreutils).
run-log-stdio: os.img disk.img disk2.img
	@echo "COM1 -> terminal + $(CURDIR)/qemu_serial.log (tee, linha a linha)"
	@rm -f "$(CURDIR)/qemu_serial.log" "$(CURDIR)/qemu.log"
	stdbuf -oL -eL $(QEMU) -vga std -netdev user,id=n0,hostfwd=tcp::8797-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
	  -drive format=raw,file=$(CURDIR)/os.img,if=ide,index=0 \
	  -drive format=raw,file=$(CURDIR)/disk.img,if=ide,index=1 \
	  -drive format=raw,file=$(CURDIR)/disk2.img,if=ide,index=2 \
	  -D "$(CURDIR)/qemu.log" -d cpu_reset,guest_errors,int \
	  -no-reboot -no-shutdown \
	  -monitor none \
	  -serial stdio 2>&1 | stdbuf -oL tee "$(CURDIR)/qemu_serial.log"

run-console: os.img disk.img disk2.img
	$(QEMU) -display curses -vga std -netdev user,id=n0,hostfwd=tcp::8797-:8797 -device ne2k_pci,netdev=n0 -boot order=c \
	  -drive format=raw,file=os.img,if=ide,index=0 \
	  -drive format=raw,file=disk.img,if=ide,index=1 \
	  -drive format=raw,file=disk2.img,if=ide,index=2

install-usb: os.img
	@echo "ATENÇÃO: isso vai SOBRESCREVER o dispositivo escolhido!"
	@echo "Use com cuidado. Exemplos de dispositivos: /dev/sdb /dev/sdc"
	@echo
	@lsblk
	@read -p "Digite o dispositivo de destino (ex: /dev/sdb): " device; \
	echo "Gravando os.img em $$device..."; \
	sudo dd if=os.img of=$$device bs=4M conv=fsync status=progress; \
	echo "Concluído. Ajuste a ordem de boot da BIOS para iniciar pelo pendrive."

# Gera ISO com GRUB (Multiboot)
iso-grub: kernel_multiboot.elf
	@if ! command -v grub-mkrescue >/dev/null 2>&1; then \
	  echo "Erro: grub-mkrescue não encontrado. Instale com:"; \
	  echo "  sudo apt-get install grub-pc-bin grub-common xorriso mtools"; \
	  exit 1; \
	fi
	rm -rf iso_root
	mkdir -p iso_root/boot/grub
	cp kernel_multiboot.elf iso_root/boot/kernel.elf
	echo 'set timeout=5' > iso_root/boot/grub/grub.cfg
	echo 'set default=0' >> iso_root/boot/grub/grub.cfg
	echo '' >> iso_root/boot/grub/grub.cfg
	echo 'menuentry "Meu SO" {' >> iso_root/boot/grub/grub.cfg
	echo '    multiboot /boot/kernel.elf' >> iso_root/boot/grub/grub.cfg
	echo '    boot' >> iso_root/boot/grub/grub.cfg
	echo '}' >> iso_root/boot/grub/grub.cfg
	grub-mkrescue -o so-grub.iso iso_root
	@echo "ISO com GRUB criado: so-grub.iso"

clean:
	rm -f *.o *.elf *.bin os.img executables.c
	rm -f shell/*.o disks/*.o kernel/*.o fs/*.o net/*.o video/*.o
	rm -rf iso_root

clean-all: clean
	rm -f disk.img disk2.img so.iso so-grub.iso
