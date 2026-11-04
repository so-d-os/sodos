# firefox/Makefile - port do Firefox para o SODOS.
#
#   make               -> build/out/SODOS.DLL  (camada de plataforma, SOX x32)
#   make test          -> FBOOT.EXE e roda no soxrun
#   make test ARGS=net -> inclui o GET HTTP
#   make nspr          -> compila o NSPR 4.35 (vendor/, baixado por setup.sh)
#   make nspr-test     -> liga PRTEST.EXE e roda no soxrun
#   make cxx           -> runtime C++ minimo + CXXTEST.EXE
#   make cxx-test      -> compila e roda o CXXTEST.EXE no soxrun
#   make libcxx        -> libc++ (subconjunto) + STLTEST.EXE
#   make libcxx-test   -> compila e roda o STLTEST.EXE no soxrun
#   make toolchain     -> valida x86_64-sodos-gcc/g++ (compila e liga SOX)
#   make clean         -> remove build/ (nao remove vendor/)
#
# Precisa do c_compiler ja compilado (make -C ../c_compiler), de gcc -mx32 e
# do soxrun. No Windows, rode por dentro do WSL (como o resto do projeto).

C    := ../c_compiler
CC   := ./sodos/sodos-cc
LINK := $(C)/build/link
OBJ  := build/obj
OUT  := build/out
RUN  := build/run

CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -Isodos/include -I$(C)/include -D__SODOS__

PORT_SRC := $(wildcard sodos/port/*.c)
PORT_OBJ := $(patsubst sodos/port/%.c,$(OBJ)/%.o,$(PORT_SRC))

.PHONY: all test nspr nspr-test nspr64 libcxx64 rust64 rust64-custom cxx cxx-test libcxx libcxx-test mfbt mfbt-test toolchain clean

all: $(OUT)/SODOS.DLL

$(OBJ)/%.o: sodos/port/%.c sodos/port/sodos_internal.h sodos/include/sodos.h
	@mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/SODOS.DLL: $(PORT_OBJ) $(C)/out/LIBCX.DLL $(C)/out/KERNEL.IMP
	@mkdir -p $(OUT)
	$(LINK) /dll /exportall /out:$@ $(PORT_OBJ) \
	  $(C)/out/LIBCX.DLL $(C)/out/KERNEL.IMP

$(OBJ)/t_fboot.o: sodos/tests/t_fboot.c sodos/include/sodos.h
	@mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/FBOOT.EXE: $(OBJ)/t_fboot.o $(OUT)/SODOS.DLL
	$(LINK) /out:$@ $(C)/build/x_crt0.o $(OBJ)/t_fboot.o \
	  $(OUT)/SODOS.DLL $(C)/out/LIBCX.DLL $(C)/out/KERNEL.IMP

test: $(OUT)/FBOOT.EXE
	@mkdir -p $(RUN)
	cp $(OUT)/FBOOT.EXE $(OUT)/SODOS.DLL $(C)/out/LIBCX.DLL $(C)/out/KERNEL.IMP $(RUN)/
	$(C)/build/soxrun -C $(RUN) FBOOT.EXE $(ARGS)

# ---- NSPR 4.35 (M1) -------------------------------------------------------
# O fonte vem de vendor/nspr-4.35 (setup.sh). O build.sh compila tudo com o
# sodos-cc e o link.sh junta PRTEST.EXE com o linker do SODOS.

nspr: $(OUT)/SODOS.DLL
	sh sodos/nspr/build.sh

nspr-test: nspr
	sh sodos/nspr/link.sh --run

# NSPR 4.39 / libsodos LP64 (SOX2): compila e liga, roda no QEMU.
nspr64:
	NSPR_BITS=64 sh sodos/nspr/build.sh
	sh sodos/nspr/link64.sh

libcxx64:
	sh sodos/libcxx64/build.sh

rust64:
	sh sodos/rust/build.sh

rust64-custom:
	sh sodos/rust/build_custom.sh

# ---- C++ minimo (M2a) -----------------------------------------------------
# Runtime proprio (new/delete, __cxa_*, construtores globais via .init_array)
# + CXXTEST. O g++ local vem de sodos/scripts/fetch-gxx.sh.

cxx:
	sh sodos/cxx/build.sh

cxx-test: cxx
	sh sodos/cxx/build.sh --run

# ---- libc++ (M2b) ---------------------------------------------------------
# Headers e runtime da libc++ 18.1.8 (setup.sh baixa em vendor/). O teste
# cobre vector/string/map/unordered_map/set/list/deque/array/tuple/optional/
# variant/chrono/algorithm/memory/functional.

libcxx:
	sh sodos/libcxx/build.sh

libcxx-test: libcxx
	sh sodos/libcxx/build.sh --run-only

# ---- mfbt (arvore do Firefox) --------------------------------------------
# Precisa de FIREFOX_DIR apontando para o fonte extraido (setup).
mfbt:
	sh sodos/mfbt/build.sh

mfbt-test: mfbt
	sh sodos/mfbt/build.sh --run-only

# ---- toolchain de destino (M2/M5) ----------------------------------------
# x86_64-sodos-gcc/g++: compila x32 e liga SOX; usado pelo configure.

toolchain:
	sh toolchain/check.sh

clean:
	rm -rf build
