# wine/Makefile - WINE.DLL (loader PE do SODOS) e EXEs de teste do Windows.
# Rodar no WSL:  make -C wine
#
#   build/WINE.DLL        DLL SOX2; tambem copiado para c_compiler/out/DRV
#                         (o disk-files do Makefile principal leva a D:\SODOS\DRV)
#   build/tests/*.EXE     EXEs PE32+ feitos com o zig (x86_64-windows-gnu)

CC_DIR = ../c_compiler
B = build
LINK64 = $(CC_DIR)/build/link64
KIMP = $(CC_DIR)/out/KERNEL.IMP
ZIG ?= $(HOME)/zig/zig

LIBC64_SRC = string ctype stdlib stdio printf scanf fpconv math time misc wchar sigfault64 setjmp64
LIBC64_OBJ = $(patsubst %,$(CC_DIR)/build/x64_%.o,$(LIBC64_SRC))

# Como a LIBC64: codigo de programa (SSE liberado), modelo large para o link64.
CFLAGS = -m64 -O2 -fno-strict-aliasing -fno-tree-loop-distribute-patterns -ffreestanding \
  -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=large -mstackrealign \
  -fno-asynchronous-unwind-tables -nostdinc -isystem $(CC_DIR)/include -Wall -Wno-unused-function

SRC = main pe k32 crt util vm obj exc file sys tables net reg misc crypt winrt pkg zip proc bashbox sig compat user gdi ctl dlg text wfont shell uimisc comctl procmisc sec map safearray ordtab ttf dwrite nt pipe fill profsave picker mf
OBJ = $(patsubst %,$(B)/%.o,$(SRC))

TESTS = HELLOW MSGBOXW WRT KUSER EXPLTEST VMTEST MAPTEST DWTEST FETCH SPAWNTEST PIPETEST GITTEST ARGV0TEST BASHPAR FSTEST
TEST_EXE = $(patsubst %,$(B)/tests/%.EXE,$(TESTS))

all: $(B)/WINE.DLL tests

$(B) $(B)/tests:
	mkdir -p $@

$(B)/%.o: dll/%.c dll/wine_int.h dll/gui_int.h dll/zip.h dll/winrt.h ../kernel/wine.h | $(B)
	gcc $(CFLAGS) -I../video -c $< -o $@

# fonte TrueType: o mesmo rasterizador do WIN, com o Latin-1
$(B)/wfont.o: ../video/wfont.c ../video/wfont.h | $(B)
	gcc $(CFLAGS) -DWFONT_LAST=255 -I../video -c $< -o $@

$(KIMP): ../kernel/kexports.c
	$(MAKE) -C $(CC_DIR) out/KERNEL.IMP

$(LIBC64_OBJ) $(LINK64):
	$(MAKE) -C $(CC_DIR) $(patsubst $(CC_DIR)/%,%,$@)

$(B)/WINE.DLL: $(OBJ) $(LIBC64_OBJ) $(LINK64) $(KIMP)
	$(LINK64) /dll /export:wine_driver /export:wine_dump /export:wine_pid_gone /out:$@ $(OBJ) $(LIBC64_OBJ) $(KIMP)
	mkdir -p $(CC_DIR)/out/DRV
	cp $@ $(CC_DIR)/out/DRV/WINE.DLL

tests: $(TEST_EXE)

$(B)/tests/GITTEST.EXE: tests/gittest.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@ -lole32 -lshell32 -luuid
	rm -f $(B)/tests/GITTEST.pdb

$(B)/tests/HELLOW.EXE: tests/hello.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/HELLOW.pdb

$(B)/tests/MSGBOXW.EXE: tests/msgbox.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@ -luser32
	rm -f $(B)/tests/MSGBOXW.pdb

clean:
	rm -rf $(B)

.PHONY: all tests clean

$(B)/tests/WRT.EXE: tests/wrt.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/WRT.pdb

$(B)/tests/KUSER.EXE: tests/kuser.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/KUSER.pdb

$(B)/tests/EXPLTEST.EXE: tests/explorer.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/EXPLTEST.pdb

$(B)/tests/VMTEST.EXE: tests/vmtest.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/VMTEST.pdb

$(B)/tests/MAPTEST.EXE: tests/maptest.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/MAPTEST.pdb

$(B)/tests/DWTEST.EXE: tests/dwtest.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@ -ldwrite
	rm -f $(B)/tests/DWTEST.pdb

$(B)/tests/FETCH.EXE: tests/fetch.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@ -lws2_32
	rm -f $(B)/tests/FETCH.pdb

$(B)/tests/SPAWNTEST.EXE: tests/spawntest.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/SPAWNTEST.pdb

$(B)/tests/PIPETEST.EXE: tests/pipetest.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/PIPETEST.pdb

$(B)/tests/ARGV0TEST.EXE: tests/argv0test.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/ARGV0TEST.pdb

$(B)/tests/BASHPAR.EXE: tests/bashpar.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/BASHPAR.pdb

$(B)/tests/FSTEST.EXE: tests/fstest.c | $(B)/tests
	$(ZIG) cc -target x86_64-windows-gnu -O2 $< -o $@
	rm -f $(B)/tests/FSTEST.pdb
