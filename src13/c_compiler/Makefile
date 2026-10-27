# c_compiler/Makefile - compilador (cc), linker (link), libc e exemplos.
#   build/  ferramentas do Linux (testes e bootstrap)
#   out/    arquivos que vao para o disco do SO (make disk-files na raiz)
#
# A libc, o CRT0 e o HELLO.EXE saem do nosso cc (long mode).
# gcc -m32 nao roda neste kernel.
LIBC_WITH ?= cc

HOSTCC = gcc
HCFLAGS = -O2 -g -Wall -Wno-unused-function -I.
B = build
O = out

TGCC = gcc
TGFLAGS = -m32 -march=i386 -mno-sse -mno-mmx -O2 -ffreestanding -fno-pic -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin \
	-fno-tree-loop-distribute-patterns -fno-jump-tables -nostdinc -Iinclude -Wall

LINK = $(B)/link
LIBC_SRC = lib/string.c lib/ctype.c lib/stdlib.c lib/stdio.c lib/printf.c lib/divdi.c
ifeq ($(LIBC_WITH),gcc)
LIBC_OBJ = $(patsubst lib/%.c,$(B)/g_%.o,$(LIBC_SRC))
CRT0_SRC_OBJ = $(B)/g_crt0.o
else
LIBC_OBJ = $(patsubst lib/%.c,$(B)/c_%.obj,$(LIBC_SRC))
CRT0_SRC_OBJ = $(B)/c_crt0.obj
endif
HEADERS = $(wildcard include/*.h)

SOXRUN = $(B)/soxrun
HCC = $(B)/cc
CC_SRC = cc/main.c cc/util.c cc/lex.c cc/pp.c cc/type.c cc/parse.c cc/gen.c

all: $(LINK) $(SOXRUN) $(HCC) $(O)/KERNEL.IMP $(O)/LIBC.DLL $(O)/CRT0.OBJ $(O)/HELLO.EXE \
	$(O)/CC.EXE $(O)/LINK.EXE $(O)/TLS.DLL $(O)/HTTPS.EXE $(O)/GUI.DLL $(O)/IMG.DLL \
	$(O)/BROWSER.EXE $(O)/IMAGEVW.EXE $(O)/VIDPLAY.EXE $(O)/FONTADV.EXE $(O)/COMMAND.EXE $(O)/HTTPD.EXE

# GUI.DLL (janela canvas do WIN) e o navegador, em varios arquivos
$(B)/gui_gui.obj: gui/gui.c include/gui.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@

$(O)/GUI.DLL: $(B)/gui_gui.obj $(O)/LIBC.DLL $(LINK)
	$(HCC) -dll -o $@ $(B)/gui_gui.obj

$(B)/img_img.obj: img/img.c include/img.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@

$(O)/IMG.DLL: $(B)/img_img.obj $(O)/LIBC.DLL $(LINK)
	$(HCC) -dll -o $@ $(B)/img_img.obj

BROWSER_OBJ = $(B)/br_main.obj $(B)/br_url.obj $(B)/br_http.obj $(B)/br_html.obj $(B)/br_js.obj \
	$(B)/br_search.obj $(B)/br_cache.obj
$(B)/br_%.obj: browser/%.c browser/browser.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@
$(B)/br_cache.obj: browser/cache.cpp browser/cache.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@

$(O)/BROWSER.EXE: $(BROWSER_OBJ) $(O)/TLS.DLL $(O)/GUI.DLL $(O)/IMG.DLL
	$(HCC) -o $@ $(BROWSER_OBJ) $(O)/TLS.DLL $(O)/GUI.DLL $(O)/IMG.DLL

# visualizador de imagem (utils/image_view.c). Nome 8.3: IMAGEVW.EXE
$(O)/IMAGEVW.EXE: ../utils/image_view.c $(O)/GUI.DLL $(O)/IMG.DLL $(HCC) $(HEADERS)
	$(HCC) -o $@ ../utils/image_view.c $(O)/GUI.DLL $(O)/IMG.DLL

# Player de video. Nome 8.3: VIDPLAY.EXE
$(O)/VIDPLAY.EXE: ../utils/videoplay.c $(O)/GUI.DLL $(O)/IMG.DLL $(HCC) $(HEADERS)
	$(HCC) -o $@ ../utils/videoplay.c $(O)/GUI.DLL $(O)/IMG.DLL

# Fontes avancadas: lista D:\SODOS\FONTS e desenha a frase em TTF.
$(O)/FONTADV.EXE: ../fonts/fontadv.c ../fonts/ttfrast.c ../fonts/ttfrast.h $(O)/GUI.DLL $(HCC) $(HEADERS)
	$(HCC) -o $@ ../fonts/fontadv.c ../fonts/ttfrast.c $(O)/GUI.DLL

# Prompt de comando. A janela preta e desenhada pelo WIN; o EXE abre outra copia.
$(O)/COMMAND.EXE: examples/command.c $(HCC) $(HEADERS)
	$(HCC) -o $@ examples/command.c

$(O)/HTTPD.EXE: examples/httpd.c $(HCC) $(HEADERS)
	$(HCC) -o $@ examples/httpd.c

# TLS.DLL: cliente TLS 1.3, compilado pelo nosso cc
TLS_OBJ = $(B)/tls_tls.obj $(B)/tls_crypto.obj $(B)/tls_p256-m.obj \
	$(B)/tls_p384.obj $(B)/tls_x509.obj $(B)/tls_roots.obj
$(B)/tls_%.obj: tls/%.c tls/crypto.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@

$(O)/TLS.DLL: $(TLS_OBJ) $(O)/LIBC.DLL $(LINK)
	$(HCC) -dll -o $@ $(TLS_OBJ)

$(O)/HTTPS.EXE: examples/https.c $(O)/TLS.DLL $(HCC) $(HEADERS)
	$(HCC) -o $@ examples/https.c $(O)/TLS.DLL

$(HCC): $(CC_SRC) cc/cc.h sox.h | $(B)
	$(HOSTCC) $(HCFLAGS) -o $@ $(CC_SRC)

# hello compilado pelo nosso cc (o HELLO.EXE acima e do gcc)
$(B)/hello.obj: examples/hello.c $(HCC) $(HEADERS)
	$(HCC) -c examples/hello.c -o $@

$(O)/HELLO2.EXE: $(B)/hello.obj $(O)/CRT0.OBJ $(O)/LIBC.DLL $(LINK)
	$(LINK) /out:$@ /map $(O)/CRT0.OBJ $(B)/hello.obj $(O)/LIBC.DLL $(O)/KERNEL.IMP

$(B) $(O):
	mkdir -p $@

$(LINK): link/link.c sox.h | $(B)
	$(HOSTCC) $(HCFLAGS) -o $@ link/link.c

$(SOXRUN): tools/soxrun.c sox.h | $(B)
	$(HOSTCC) -no-pie -fno-pie -mstackrealign -D_FILE_OFFSET_BITS=64 $(HCFLAGS) -o $@ tools/soxrun.c

$(O)/KERNEL.IMP: ../kernel/kexports.c | $(O)
	( echo "LIBRARY KERNEL"; sed -n 's/^ *EXP(\([A-Za-z_0-9]*\)).*/\1/p' $< ) > $@

$(B)/g_%.o: lib/%.c $(HEADERS) | $(B)
	$(TGCC) $(TGFLAGS) -c $< -o $@

$(B)/c_%.obj: lib/%.c $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@

$(O)/CRT0.OBJ: $(CRT0_SRC_OBJ) | $(O)
	cp $< $@

$(O)/LIBC.DLL: $(LIBC_OBJ) $(O)/KERNEL.IMP $(LINK)
	$(LINK) /dll /exportall /out:$@ $(LIBC_OBJ) $(O)/KERNEL.IMP

$(B)/g_hello.o: examples/hello.c $(HEADERS) | $(B)
	$(TGCC) $(TGFLAGS) -c $< -o $@

$(O)/HELLO.EXE: $(B)/hello.obj $(O)/CRT0.OBJ $(O)/LIBC.DLL $(LINK)
	$(LINK) /out:$@ /map $(O)/CRT0.OBJ $(B)/hello.obj $(O)/LIBC.DLL $(O)/KERNEL.IMP

# ---- CC.EXE e LINK.EXE para o SO, compilados pelo nosso cc (estagio 1) ----
CC_OBJ1 = $(patsubst cc/%.c,$(B)/s1_%.obj,$(CC_SRC))

$(B)/s1_%.obj: cc/%.c cc/cc.h sox.h $(HCC) $(HEADERS)
	$(HCC) -I. -c $< -o $@

$(B)/s1_link.obj: link/link.c sox.h $(HCC) $(HEADERS)
	$(HCC) -I. -c $< -o $@

$(O)/CC.EXE: $(CC_OBJ1) $(O)/CRT0.OBJ $(O)/LIBC.DLL $(LINK)
	$(LINK) /out:$@ $(O)/CRT0.OBJ $(CC_OBJ1) $(O)/LIBC.DLL $(O)/KERNEL.IMP

$(O)/LINK.EXE: $(B)/s1_link.obj $(O)/CRT0.OBJ $(O)/LIBC.DLL $(LINK)
	$(LINK) /out:$@ $(O)/CRT0.OBJ $(B)/s1_link.obj $(O)/LIBC.DLL $(O)/KERNEL.IMP

# Estagio 2: CC.EXE (rodando no soxrun, numa pasta igual a raiz do D:)
# recompila o cc; os .OBJ e o CC2.EXE tem que sair identicos ao estagio 1.
OSD = $(B)/os
CC_UP = MAIN UTIL LEX PP TYPE PARSE GEN
selfhost: $(O)/CC.EXE $(O)/LINK.EXE $(SOXRUN)
	rm -rf $(OSD) && mkdir -p $(OSD)
	cp $(O)/CC.EXE $(O)/LINK.EXE $(O)/LIBC.DLL $(O)/CRT0.OBJ $(O)/KERNEL.IMP $(OSD)/
	@for f in include/*.h cc/*.c cc/cc.h sox.h link/link.c examples/hello.c; do \
	  cp $$f $(OSD)/$$(basename $$f | tr a-z A-Z); done
	@for m in $(CC_UP); do \
	  echo "CC.EXE -c $$m.C"; \
	  $(SOXRUN) -C $(OSD) CC.EXE -c $$m.C -o $$m.OBJ || exit 1; \
	  l=$$(echo $$m | tr A-Z a-z); cmp $(OSD)/$$m.OBJ $(B)/s1_$$l.obj || exit 1; \
	done
	$(SOXRUN) -C $(OSD) LINK.EXE /out:CC2.EXE CRT0.OBJ $(addsuffix .OBJ,$(CC_UP)) LIBC.DLL KERNEL.IMP
	cmp $(OSD)/CC2.EXE $(O)/CC.EXE
	$(SOXRUN) -C $(OSD) CC.EXE -v HELLO.C
	$(SOXRUN) -C $(OSD) HELLO.EXE auto-hospedado
	@echo "selfhost ok: CC.EXE compila a si mesmo e gera o mesmo binario"

# tests/t_*.c: compilados pelo cc, linkados e rodados no soxrun
TESTS = $(patsubst tests/%.c,$(B)/%.exe,$(wildcard tests/t_*.c))

$(B)/%.obj: tests/%.c $(HCC) $(HEADERS)
	$(HCC) -c $< -o $@

$(B)/%.exe: $(B)/%.obj $(O)/CRT0.OBJ $(O)/LIBC.DLL $(LINK)
	$(LINK) /out:$@ $(O)/CRT0.OBJ $< $(O)/LIBC.DLL $(O)/KERNEL.IMP

test: all $(O)/HELLO2.EXE $(TESTS)
	$(SOXRUN) -C $(O) HELLO2.EXE um "dois tres"
	@for t in $(TESTS); do echo "== $$t"; cp $$t $(O)/TEST.EXE; $(SOXRUN) -C $(O) TEST.EXE || exit 1; done
	$(HCC) -o $(B)/vmain.exe examples/vmain.cpp examples/vec.cpp
	cp $(B)/vmain.exe $(O)/TEST.EXE
	$(SOXRUN) -C $(O) TEST.EXE

# Exemplos compilados pelo CC.EXE (como no SO): multi-arquivo e DLL
EXD = $(B)/ex
examples: $(O)/CC.EXE $(O)/LINK.EXE $(SOXRUN)
	rm -rf $(EXD) && mkdir -p $(EXD)
	cp $(O)/CC.EXE $(O)/LINK.EXE $(O)/LIBC.DLL $(O)/CRT0.OBJ $(O)/KERNEL.IMP $(EXD)/
	@for f in include/*.h examples/*.c examples/*.h examples/*.cpp; do \
	  cp $$f $(EXD)/$$(basename $$f | tr a-z A-Z); done
	$(SOXRUN) -C $(EXD) CC.EXE CALC.C CALCOPS.C
	$(SOXRUN) -C $(EXD) CALC.EXE "2*(3+4)" "100/7" "-5+2*-3" "1/0" "2+"
	$(SOXRUN) -C $(EXD) CC.EXE -dll MATHX.C
	$(SOXRUN) -C $(EXD) CC.EXE USEMATH.C MATHX.DLL
	$(SOXRUN) -C $(EXD) USEMATH.EXE
	$(SOXRUN) -C $(EXD) CC.EXE DYNLOAD.C
	$(SOXRUN) -C $(EXD) DYNLOAD.EXE
	! $(SOXRUN) -C $(EXD) DYNLOAD.EXE NADA.DLL
	$(SOXRUN) -C $(EXD) CC.EXE VMAIN.CPP VEC.CPP
	$(SOXRUN) -C $(EXD) VMAIN.EXE
	@echo "examples ok"

clean:
	rm -rf $(B) $(O)

.PHONY: all clean test selfhost examples
