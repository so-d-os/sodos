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

# gcc -mx32: long mode + ponteiros 32 bits (ABI do SO). Preferir a -m32.
XGFLAGS = -mx32 -march=x86-64 -mno-sse -mno-mmx -mno-red-zone -O2 -ffreestanding \
	-fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-builtin -fno-jump-tables -nostdinc -Iinclude -Wall

LINK = $(B)/link
LIBC_SRC = lib/string.c lib/ctype.c lib/stdlib.c lib/stdio.c lib/printf.c lib/divdi.c \
	lib/fpconv.c lib/math.c lib/scanf.c lib/time.c lib/setjmp.c lib/misc.c
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

all: $(LINK) $(SOXRUN) $(HCC) $(O)/KERNEL.IMP $(O)/LIBC.DLL $(O)/CRT0.OBJ $(O)/HELLO.EXE $(O)/CRASHT.EXE \
	$(O)/CC.EXE $(O)/LINK.EXE $(O)/TLS.DLL $(O)/HTTPS.EXE $(O)/GUI.DLL $(O)/IMG.DLL \
	$(O)/BROWSER.EXE $(O)/IMAGEVW.EXE $(O)/VIDPLAY.EXE $(O)/FONTADV.EXE $(O)/COMMAND.EXE $(O)/HTTPD.EXE \
	$(O)/WIDGETS.EXE $(O)/EXPLORER.EXE $(O)/VIDEOCFG.EXE $(O)/IMGBIG.DLL $(O)/AIPANEL.EXE $(O)/MENUCFG.EXE \
	$(O)/WGET.EXE $(O)/BAT.EXE $(O)/POSIX.DLL $(O)/LIBCX.DLL $(O)/XHELLO.EXE \
	$(O)/TERM.DLL $(O)/TTERM.EXE $(O)/GLIB.DLL $(O)/TGLIB.EXE \
	$(O)/POSIXX.DLL $(O)/MC.EXE $(O)/TOP.EXE $(O)/COBRA.EXE $(O)/WIFI.EXE $(O)/GIT.EXE \
	$(O)/AGENT.EXE $(O)/LLAMA.EXE \
	drivers

# GUI.DLL (janela canvas do WIN) e o navegador, em varios arquivos
$(B)/gui_gui.obj: gui/gui.c include/gui.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@

$(B)/gui_widgets.obj: gui/widgets.c include/widgets.h include/gui.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@

$(O)/GUI.DLL: $(B)/gui_gui.obj $(B)/gui_widgets.obj $(O)/LIBC.DLL $(LINK)
	$(HCC) -dll -o $@ $(B)/gui_gui.obj $(B)/gui_widgets.obj

$(B)/img_img.obj: img/img.c include/img.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@

$(O)/IMG.DLL: $(B)/img_img.obj $(O)/LIBC.DLL $(LINK)
	$(HCC) -dll -o $@ $(B)/img_img.obj

# Mesmo decodificador, com IMG_MAXS=1920: papel de parede em resolucao cheia.
$(B)/img_img_big.obj: img/img_big.c img/img.c include/img.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c $< -o $@

$(O)/IMGBIG.DLL: $(B)/img_img_big.obj $(O)/LIBC.DLL $(LINK)
	$(HCC) -dll -o $@ $(B)/img_img_big.obj

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

# Painel de demonstracao dos widgets (botoes, checks, lista, progresso).
$(O)/WIDGETS.EXE: ../utils/widgets_demo.c $(O)/GUI.DLL include/widgets.h $(HCC) $(HEADERS)
	$(HCC) -o $@ ../utils/widgets_demo.c $(O)/GUI.DLL

# Gerenciador de arquivos do WIN (navegacao entre pastas e unidades).
$(O)/EXPLORER.EXE: ../utils/explorer.c $(O)/GUI.DLL include/widgets.h $(HCC) $(HEADERS)
	$(HCC) -o $@ ../utils/explorer.c $(O)/GUI.DLL

# Configurador de tela: modelo retro/avancado, cor e imagem de fundo.
$(O)/VIDEOCFG.EXE: ../utils/video_cfg.c $(O)/GUI.DLL $(O)/IMGBIG.DLL include/widgets.h $(HCC) $(HEADERS)
	$(HCC) -o $@ ../utils/video_cfg.c $(O)/GUI.DLL $(O)/IMGBIG.DLL

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

$(O)/WGET.EXE: examples/wget.c $(O)/TLS.DLL $(HCC) $(HEADERS) $(O)/KERNEL.IMP
	$(HCC) -o $@ examples/wget.c $(O)/TLS.DLL

$(O)/AGENT.EXE: agent/agent.c $(O)/TLS.DLL $(HCC) $(HEADERS) $(O)/KERNEL.IMP
	$(HCC) -o $@ agent/agent.c $(O)/TLS.DLL

$(O)/BAT.EXE: examples/bat.c $(HCC) $(HEADERS) $(O)/KERNEL.IMP $(O)/LIBC.DLL
	$(HCC) -o $@ examples/bat.c

# LLAMA.EXE: LLM GGUF local. gcc -mx32 com SSE2 (o kernel liga o SSE).
LLGFLAGS = $(filter-out -mno-sse -mno-mmx -O2,$(XGFLAGS)) -msse2 -O3 -fno-math-errno
$(B)/x_llama.o: llama/llama.c $(HEADERS) | $(B)
	$(HOSTCC) $(LLGFLAGS) -c $< -o $@
$(O)/LLAMA.EXE: $(B)/x_crt0.o $(B)/x_llama.o $(O)/LIBCX.DLL $(LINK) $(O)/KERNEL.IMP
	$(LINK) /out:$@ $(B)/x_crt0.o $(B)/x_llama.o $(O)/LIBCX.DLL $(O)/KERNEL.IMP

# posix-lite: subset POSIX sobre k_* (port MC)
$(B)/posix_posix.obj: ../libs/posix-lite/posix.c include/unistd.h include/dirent.h \
		include/fcntl.h include/sys/stat.h include/sys/types.h $(HCC) $(HEADERS) | $(B)
	$(HCC) -c ../libs/posix-lite/posix.c -o $@

$(O)/POSIX.DLL: $(B)/posix_posix.obj $(O)/LIBC.DLL $(LINK)
	$(HCC) -dll -o $@ $(B)/posix_posix.obj

# gcc -mx32 -> ELF -> SOX. CRT0/LIBCX em SysV (compativel com k_* e com o gcc).
# LIBC.DLL do cc continua para apps do cc (ABI de pilha).
XLIBC_OBJ = $(patsubst lib/%.c,$(B)/x_%.o,$(LIBC_SRC))
$(B)/x_%.o: lib/%.c $(HEADERS) | $(B)
	$(HOSTCC) $(XGFLAGS) -c $< -o $@
$(B)/x_crt0.o: lib/crt0.c $(HEADERS) | $(B)
	$(HOSTCC) $(XGFLAGS) -c $< -o $@
$(B)/x_hello.o: examples/xhello.c $(HEADERS) | $(B)
	$(HOSTCC) $(XGFLAGS) -c $< -o $@
$(O)/LIBCX.DLL: $(XLIBC_OBJ) $(O)/KERNEL.IMP $(LINK)
	$(LINK) /dll /exportall /out:$@ $(XLIBC_OBJ) $(O)/KERNEL.IMP
$(O)/XHELLO.EXE: $(B)/x_crt0.o $(B)/x_hello.o $(O)/LIBCX.DLL $(LINK) $(O)/KERNEL.IMP
	$(LINK) /out:$@ /map $(B)/x_crt0.o $(B)/x_hello.o $(O)/LIBCX.DLL $(O)/KERNEL.IMP

# term shim (ncurses subset) — gcc -mx32
$(B)/x_term.o: ../libs/term/term.c include/curses.h $(HEADERS) | $(B)
	$(HOSTCC) $(XGFLAGS) -c ../libs/term/term.c -o $@
$(O)/TERM.DLL: $(B)/x_term.o $(O)/LIBCX.DLL $(LINK) $(O)/KERNEL.IMP
	$(LINK) /dll /exportall /out:$@ $(B)/x_term.o $(O)/LIBCX.DLL $(O)/KERNEL.IMP
$(B)/x_tterm.o: examples/tterm.c include/curses.h $(HEADERS) | $(B)
	$(HOSTCC) $(XGFLAGS) -c examples/tterm.c -o $@
$(O)/TTERM.EXE: $(B)/x_crt0.o $(B)/x_tterm.o $(O)/TERM.DLL $(O)/LIBCX.DLL $(LINK)
	$(LINK) /out:$@ $(B)/x_crt0.o $(B)/x_tterm.o $(O)/TERM.DLL $(O)/LIBCX.DLL $(O)/KERNEL.IMP

# glib subset — gcc -mx32
$(B)/x_glib.o: ../libs/glib/glib.c include/glib.h $(HEADERS) | $(B)
	$(HOSTCC) $(XGFLAGS) -c ../libs/glib/glib.c -o $@
$(O)/GLIB.DLL: $(B)/x_glib.o $(O)/LIBCX.DLL $(LINK) $(O)/KERNEL.IMP
	$(LINK) /dll /exportall /out:$@ $(B)/x_glib.o $(O)/LIBCX.DLL $(O)/KERNEL.IMP
$(B)/x_tglib.o: examples/tglib.c include/glib.h $(HEADERS) | $(B)
	$(HOSTCC) $(XGFLAGS) -c examples/tglib.c -o $@
$(O)/TGLIB.EXE: $(B)/x_crt0.o $(B)/x_tglib.o $(O)/GLIB.DLL $(O)/LIBCX.DLL $(LINK)
	$(LINK) /out:$@ $(B)/x_crt0.o $(B)/x_tglib.o $(O)/GLIB.DLL $(O)/LIBCX.DLL $(O)/KERNEL.IMP

# POSIX SysV (gcc -mx32)
$(B)/x_posix.o: ../libs/posix-lite/posix.c include/unistd.h include/dirent.h \
		include/fcntl.h include/sys/stat.h $(HEADERS) | $(B)
	$(HOSTCC) $(XGFLAGS) -c ../libs/posix-lite/posix.c -o $@
$(O)/POSIXX.DLL: $(B)/x_posix.o $(O)/LIBCX.DLL $(LINK) $(O)/KERNEL.IMP
	$(LINK) /dll /exportall /out:$@ $(B)/x_posix.o $(O)/LIBCX.DLL $(O)/KERNEL.IMP

# MC.EXE (../mc): file manager de dois paineis, so com LIBC.DLL. No SO o
# BUILD.BAT da pasta compila os mesmos fontes com o CC.EXE.
MC_SRC = ../mc/mc.c ../mc/scr.c ../mc/panel.c ../mc/ops.c ../mc/view.c
$(O)/MC.EXE: $(MC_SRC) ../mc/mc.h $(HCC) $(HEADERS) $(O)/KERNEL.IMP $(O)/LIBC.DLL
	$(HCC) -o $@ $(MC_SRC)

# TOP.EXE (../top): programas e mapa da memoria no padrao do SPECIAL. BUILD.BAT idem.
TOP_SRC = ../top/top.c ../top/tela.c ../top/dados.c ../top/procs.c ../top/mapa.c ../top/video.c ../top/cpu.c
$(O)/TOP.EXE: $(TOP_SRC) ../top/top.h $(HCC) $(HEADERS) $(O)/KERNEL.IMP $(O)/LIBC.DLL
	$(HCC) -o $@ $(TOP_SRC)

# COBRA.EXE (../cobra): jogo da cobrinha em modo texto. BUILD.BAT idem.
$(O)/COBRA.EXE: ../cobra/cobra.cpp $(HCC) $(HEADERS) $(O)/KERNEL.IMP $(O)/LIBC.DLL
	$(HCC) -o $@ ../cobra/cobra.cpp

# WIFI.EXE (../wifi): as redes do WIFI.CFG numa tela no padrao do TOP.
# Modo texto, so KERNEL e LIBC.DLL (sem GUI.DLL). BUILD.BAT idem.
$(O)/WIFI.EXE: ../wifi/wifi.cpp $(HCC) $(HEADERS) $(O)/KERNEL.IMP $(O)/LIBC.DLL
	$(HCC) -o $@ ../wifi/wifi.cpp

# GIT.EXE (../git): clone/pull raso por HTTPS, status e log. BUILD.BAT idem.
GIT_SRC = ../git/git.c ../git/http.c ../git/pack.c ../git/sha1.c ../git/zinf.c
$(O)/GIT.EXE: $(GIT_SRC) ../git/git.h $(O)/TLS.DLL $(HCC) $(HEADERS) $(O)/KERNEL.IMP $(O)/LIBC.DLL
	$(HCC) -o $@ $(GIT_SRC) $(O)/TLS.DLL

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

# ---- SOX2 / ABI LP64 (primeiro marco) ----
LINK64 = $(B)/link64
CRT64 = $(B)/x_crt0_64.o

$(LINK64): link/link64.c sox.h | $(B)
	$(HOSTCC) $(HCFLAGS) -o $@ link/link64.c

$(CRT64): lib/crt0_64.S | $(B)
	gcc -m64 -c $< -o $@

$(B)/hello64.o: lib/hello64.c | $(B)
	gcc -m64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone \
	  -mcmodel=large -nostdinc -isystem include -c $< -o $@

$(O)/HELLO64.EXE: $(CRT64) $(B)/hello64.o $(LINK64) $(O)/KERNEL.IMP
	$(LINK64) /out:$@ $(CRT64) $(B)/hello64.o $(O)/KERNEL.IMP

hello64: $(O)/HELLO64.EXE

# Drivers de placa de video (docs/GPU.md): DLL SOX2 que roda no contexto do
# kernel, entao sem SSE, sem red zone e sem libc. Ficam em out/DRV para o
# disk-files levar a D:\SODOS\DRV (e nao a raiz do D:).
DRV = $(O)/DRV
DRVCC = gcc -m64 -O2 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone \
	-mno-sse -mno-mmx -mcmodel=large -fno-asynchronous-unwind-tables \
	-fno-tree-loop-distribute-patterns -I../video

$(DRV):
	mkdir -p $@

$(B)/drv_gputest.o: ../drivers/gputest/gputest.c ../video/gpu.h | $(B)
	$(DRVCC) -c $< -o $@
$(B)/drv_gpufail.o: ../drivers/gputest/gputest.c ../video/gpu.h | $(B)
	$(DRVCC) -DGPU_FALHA=20 -c $< -o $@

$(DRV)/GPUTEST.DLL: $(B)/drv_gputest.o $(LINK64) | $(DRV)
	$(LINK64) /dll /export:gpu_driver /out:$@ $(B)/drv_gputest.o
$(DRV)/GPUFAIL.DLL: $(B)/drv_gpufail.o $(LINK64) | $(DRV)
	$(LINK64) /dll /export:gpu_driver /out:$@ $(B)/drv_gpufail.o

$(B)/drv_virtgpu.o: ../drivers/virtgpu/virtgpu.c ../video/gpu.h | $(B)
	$(DRVCC) -c $< -o $@
$(DRV)/VIRTGPU.DLL: $(B)/drv_virtgpu.o $(LINK64) | $(DRV)
	$(LINK64) /dll /export:gpu_driver /out:$@ $(B)/drv_virtgpu.o

$(B)/drv_amdgop.o: ../drivers/amdgop/amdgop.c ../video/gpu.h | $(B)
	$(DRVCC) -c $< -o $@
$(DRV)/AMDGOP.DLL: $(B)/drv_amdgop.o $(LINK64) | $(DRV)
	$(LINK64) /dll /export:gpu_driver /out:$@ $(B)/drv_amdgop.o

$(B)/drv_amdcur.o: ../drivers/amdgop/amdgop.c ../video/gpu.h | $(B)
	$(DRVCC) -DAMD_CURSOR -c $< -o $@
$(DRV)/AMDCUR.DLL: $(B)/drv_amdcur.o $(LINK64) | $(DRV)
	$(LINK64) /dll /export:gpu_driver /out:$@ $(B)/drv_amdcur.o

drivers: $(DRV)/GPUTEST.DLL $(DRV)/GPUFAIL.DLL $(DRV)/VIRTGPU.DLL $(DRV)/AMDGOP.DLL $(DRV)/AMDCUR.DLL

# LIBCX64 estatica: mesma libc, compilada -m64, ligada no HELLO64C.
LIBC64_SRC = string ctype stdlib stdio printf scanf fpconv math time misc wchar setjmp64 ctors64
LIBC64_OBJ = $(patsubst %,$(B)/x64_%.o,$(LIBC64_SRC))

$(B)/x64_%.o: lib/%.c | $(B)
	gcc -m64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone \
	  -mcmodel=large -mstackrealign -fno-asynchronous-unwind-tables -nostdinc -isystem include -c $< -o $@

$(B)/x_crt0_64libc.o: lib/crt0.c | $(B)
	gcc -m64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone \
	  -mcmodel=large -fno-asynchronous-unwind-tables -nostdinc -isystem include -c $< -o $@

$(B)/hello64c.o: lib/hello64c.c | $(B)
	gcc -m64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-red-zone \
	  -mcmodel=large -fno-asynchronous-unwind-tables -nostdinc -isystem include -c $< -o $@

$(O)/HELLO64C.EXE: $(B)/x_crt0_64libc.o $(B)/hello64c.o $(LIBC64_OBJ) $(LINK64) $(O)/KERNEL.IMP
	$(LINK64) /out:$@ $(B)/x_crt0_64libc.o $(B)/hello64c.o $(LIBC64_OBJ) $(O)/KERNEL.IMP

hello64c: $(O)/HELLO64C.EXE

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

# CRASHT.EXE: excecao de proposito, para testar o CRASH.LOG do kernel (isr.c)
$(B)/crasht.obj: examples/crasht.c $(HCC) $(HEADERS)
	$(HCC) -c examples/crasht.c -o $@

$(O)/CRASHT.EXE: $(B)/crasht.obj $(O)/CRT0.OBJ $(O)/LIBC.DLL $(LINK)
	$(LINK) /out:$@ /map $(O)/CRT0.OBJ $(B)/crasht.obj $(O)/LIBC.DLL $(O)/KERNEL.IMP

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

# t_posix precisa de POSIX.DLL (e o DLL no cwd do soxrun)
$(B)/t_posix.exe: $(B)/t_posix.obj $(O)/CRT0.OBJ $(O)/LIBC.DLL $(O)/POSIX.DLL $(LINK)
	$(LINK) /out:$@ $(O)/CRT0.OBJ $< $(O)/LIBC.DLL $(O)/POSIX.DLL $(O)/KERNEL.IMP

test: all $(O)/HELLO2.EXE $(TESTS)
	$(SOXRUN) -C $(O) HELLO2.EXE um "dois tres"
	@for t in $(TESTS); do echo "== $$t"; cp $$t $(O)/TEST.EXE; $(SOXRUN) -C $(O) TEST.EXE || exit 1; done
	$(HCC) -o $(B)/vmain.exe examples/vmain.cpp examples/vec.cpp
	cp $(B)/vmain.exe $(O)/TEST.EXE
	$(SOXRUN) -C $(O) TEST.EXE
	@echo "== XHELLO (gcc -mx32)"
	$(SOXRUN) -C $(O) XHELLO.EXE gcc-x32 ok
	@echo "== TTERM (term shim)"
	$(SOXRUN) -C $(O) TTERM.EXE
	@echo "== TGLIB (glib subset)"
	$(SOXRUN) -C $(O) TGLIB.EXE
	@echo "== MC.EXE auto"
	$(SOXRUN) -C $(O) MC.EXE auto

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

# Configuracao do menu Iniciar: posicao, painel e transparencia.
$(O)/MENUCFG.EXE: ../utils/menu_cfg.c $(O)/GUI.DLL include/widgets.h $(HCC) $(HEADERS)
	$(HCC) -o $@ ../utils/menu_cfg.c $(O)/GUI.DLL

# Painel de IA: janela docada na lateral direita (chat).
# O painel inclui o ai_client.c (fonte unica): um objeto, como no AGENT.
# TLS carregado com k_dll_load em tempo de execucao (o linker do SODOS nao
# consegue GUI.DLL + TLS.DLL no mesmo EXE).
$(O)/AIPANEL.EXE: ../utils/ai_panel.c agent/ai_client.c $(O)/GUI.DLL $(O)/TLS.DLL include/widgets.h include/ai_client.h $(HCC) $(HEADERS) $(O)/KERNEL.IMP
	$(HCC) -o $@ ../utils/ai_panel.c $(O)/GUI.DLL
