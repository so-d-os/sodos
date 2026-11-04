# Executar a partir desta pasta; usa a ABI nativa do compilador SODOS.
SDK = ../../src13/c_compiler

all:
	$(MAKE) -C $(SDK) build/cc out/CRT0.OBJ out/KERNEL.IMP out/LIBC.DLL out/GUI.DLL
	cd $(SDK) && build/cc -o ../../src/testris/TETRIS.EXE ../../src/testris/tetris.cpp out/GUI.DLL

.PHONY: all
