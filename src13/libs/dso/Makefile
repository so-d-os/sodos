# libs/dso/Makefile - bibliotecas compartilhadas do SODOS (ELF .so, ABI LP64).
#
#   make        -> build/sysroot/lib: crt1.o, crtdso.o, libc.so.6 e os nomes padrao
#                  (libm.so.6, libpthread.so.0, libdl.so.2, librt.so.1, libutil.so.1)
#   make cxx    -> libc++.so.1 e libc++abi.so.1 (precisa de firefox/vendor/libcxx-18.1.8)
#   make test   -> programas de teste em build/test (rodam no SODOS; ver README.md)
#   make clean
#
# A libc.so.6 junta, em codigo PIC, as mesmas fontes da libc estatica:
# c_compiler/lib (libc), libs/posix-lite (arquivos) e firefox/sodos/port
# (pthreads, mmap, rede, ABI do Linux), mais start.c e dl.c daqui. Como na musl,
# tudo fica em um arquivo so; libm, libpthread, libdl, librt e libutil existem
# pelo nome, para "-lm" e DT_NEEDED de programas portados resolverem.
#
# No SODOS as bibliotecas ficam em \LIB (D:\LIB\libc.so.6). Quem carrega e o
# kernel (kernel/elf.c).

S    := ../..
C    := $(S)/c_compiler
FX   := $(S)/firefox
B    := build
OBJ  := $(B)/obj
LIB  := $(B)/sysroot/lib
T    := $(B)/test

CC64  := $(FX)/sodos/sodos-cc64
CXX64 := $(FX)/sodos/sodos-cxxlib64
GXX   := $(FX)/toolchain/bin/x86_64-sodos-g++64
GCC   := $(FX)/toolchain/bin/x86_64-sodos-gcc64
# A libc++ 18 pede g++ >= 13: o mesmo gcc local do build do Firefox, se existir.
GCC13 ?= $(HOME)/ffsrc/gcc-13
ifneq ($(wildcard $(GCC13)/bin/g++),)
export PATH := $(GCC13)/bin:$(PATH)
export LD_LIBRARY_PATH := $(GCC13)/lib64:$(LD_LIBRARY_PATH)
endif
LIBGCC := $(shell gcc -print-libgcc-file-name)

PIC := -fPIC -O2 -DSODOS_DYNAMIC
# Como as x64_*.o da libc estatica (c_compiler/Makefile), mas PIC.
LIBC_CC := gcc -m64 -ffreestanding $(PIC) -ftls-model=initial-exec -fno-stack-protector \
	-mno-red-zone -mstackrealign -fno-asynchronous-unwind-tables \
	-fno-tree-loop-distribute-patterns -nostdinc -isystem $(C)/include
PORT_CC := $(CC64) $(PIC) -I $(FX)/sodos/include -I $(C)/include

LDSO := ld -shared --hash-style=both -z max-page-size=0x1000 -z noexecstack -z notext \
	--no-eh-frame-hdr --strip-debug

LIBC_SRC := string ctype stdlib stdio printf scanf fpconv math time misc wchar setjmp64
PORT_SRC := $(notdir $(basename $(wildcard $(FX)/sodos/port/*.c)))
# A ordem importa: port, posix e libc repetem algumas funcoes (getpid, pipe,
# fcntl...) e vale a primeira, como no link estatico (toolchain/sodos-gcc.py).
LIBC_OBJ := $(OBJ)/start.o $(OBJ)/dl.o $(patsubst %,$(OBJ)/p_%.o,$(PORT_SRC)) \
	$(OBJ)/posix.o $(patsubst %,$(OBJ)/c_%.o,$(LIBC_SRC))

STUBS := libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 libutil.so.1

.PHONY: all cxx test test-cxx check clean
all: $(LIB)/crt1.o $(LIB)/crtdso.o $(LIB)/libkernel.so $(LIB)/libc.so.6 $(patsubst %,$(LIB)/%,$(STUBS))

$(OBJ) $(LIB) $(T):
	mkdir -p $@

$(OBJ)/c_%.o: $(C)/lib/%.c | $(OBJ)
	$(LIBC_CC) -c $< -o $@

$(OBJ)/p_%.o: $(FX)/sodos/port/%.c $(FX)/sodos/port/sodos_internal.h | $(OBJ)
	$(PORT_CC) -c $< -o $@

$(OBJ)/posix.o: $(S)/libs/posix-lite/posix.c | $(OBJ)
	$(PORT_CC) -c $< -o $@

$(OBJ)/%.o: %.c | $(OBJ)
	$(PORT_CC) -c $< -o $@

$(LIB)/crt1.o: crt1.S | $(LIB)
	gcc -m64 -c $< -o $@

$(LIB)/crtdso.o: crtdso.c | $(LIB)
	$(PORT_CC) -c $< -o $@

# So para o link: um simbolo por export do kernel. O carregador nunca procura
# este arquivo; os k_* sao resolvidos direto na tabela do kernel.
$(OBJ)/kernel.s: $(S)/kernel/kexports.c | $(OBJ)
	( echo ".text"; sed -n 's/^ *EXP(\([A-Za-z_0-9]*\)).*/.globl \1\n.type \1,@function\n\1: ret/p' $< ; \
	  echo '.section .note.GNU-stack,"",@progbits' ) > $@

$(LIB)/libkernel.so: $(OBJ)/kernel.s | $(LIB)
	gcc -m64 -c $< -o $(OBJ)/kernel.o
	$(LDSO) -soname libkernel.so -o $@ $(OBJ)/kernel.o

$(LIB)/libc.so.6: $(LIBC_OBJ) $(LIB)/crtdso.o $(LIB)/libkernel.so | $(LIB)
	$(LDSO) --allow-multiple-definition -soname libc.so.6 -o $@ $(LIB)/crtdso.o $(LIBC_OBJ) \
	  $(LIB)/libkernel.so $(LIBGCC)
	ln -sf libc.so.6 $(LIB)/libc.so

$(OBJ)/empty.o: | $(OBJ)
	echo "" | gcc -m64 -x c -c - -o $@

# libNOME.so.N -> libNOME.so para o "-lNOME" do link.
$(LIB)/lib%: $(OBJ)/empty.o $(LIB)/libc.so.6
	$(LDSO) -soname $(notdir $@) -o $@ $(OBJ)/empty.o $(LIB)/libc.so.6
	ln -sf $(notdir $@) $(LIB)/$(basename $(notdir $@))

# ---- C++: libc++ 18.1.8 + runtime (new/delete, guards) --------------------
L := $(FX)/vendor/libcxx-18.1.8
CXX_SRC := string algorithm stdexcept exception verbose_abort new new_handler new_helpers \
	typeinfo memory functional hash vector optional any system_error error_category \
	thread mutex mutex_destructor condition_variable condition_variable_destructor \
	call_once shared_mutex future locale ios ios.instantiations iostream ostream \
	fstream chrono regex
CXX_OBJ := $(patsubst %,$(OBJ)/xx_%.o,$(CXX_SRC)) $(OBJ)/xx_cxx_runtime.o
CXXFLAGS := $(PIC) -std=c++20 -D_LIBCPP_BUILDING_LIBRARY -D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS \
	-fno-exceptions -I$(L)/src -Wno-deprecated-declarations -Wno-alloc-size-larger-than \
	-Wno-return-type -Wno-noreturn -Wno-cpp

cxx: all $(LIB)/libc++.so.1 $(LIB)/libc++abi.so.1

$(OBJ)/xx_cxx_runtime.o: $(FX)/sodos/cxx/cxx_runtime.cpp | $(OBJ)
	$(CXX64) $(PIC) -c $< -o $@

$(OBJ)/xx_%.o: $(L)/src/%.cpp | $(OBJ)
	$(CXX64) $(CXXFLAGS) -c $< -o $@

$(LIB)/libc++.so.1: $(CXX_OBJ) $(LIB)/libc.so.6
	$(LDSO) -soname libc++.so.1 -o $@ $(LIB)/crtdso.o $(CXX_OBJ) $(LIB)/libc.so.6 $(LIBGCC)
	ln -sf libc++.so.1 $(LIB)/libc++.so

# Sobe o SODOS no QEMU (sem janela) e roda os testes la dentro.
check: test
	sh tests/run-qemu.sh

clean:
	rm -rf $(B)

# ---- testes (rodam dentro do SODOS: tests/run-qemu.sh) ---------------------
DYN := SODOS_DYNAMIC=1
test: all $(T)/DSOTEST.EXE $(T)/libtfoo.so.1 $(T)/libtplug.so
test-cxx: cxx $(T)/CXXDSO.EXE

$(T)/libtfoo.so.1: tests/t_foo.c $(LIB)/libc.so.6 | $(T)
	$(DYN) $(GCC) -O2 -fPIC -shared -Wl,-soname,libtfoo.so.1 -o $@ $< -lm
	ln -sf libtfoo.so.1 $(T)/libtfoo.so

$(T)/libtplug.so: tests/t_plug.c $(T)/libtfoo.so.1 | $(T)
	$(DYN) $(GCC) -O2 -fPIC -shared -o $@ $< -L$(T) -ltfoo

# -rdynamic: o plugin chama host_value() do programa.
$(T)/DSOTEST.EXE: tests/t_dso.c $(T)/libtfoo.so.1 | $(T)
	$(DYN) $(GCC) -O2 -rdynamic -o $@ $< -L$(T) -ltfoo -lpthread -ldl

$(T)/CXXDSO.EXE: tests/t_cxx.cpp $(LIB)/libc++.so.1 | $(T)
	$(DYN) $(GXX) -O2 -std=c++20 -o $@ $<
