/* so.h - Funcoes do kernel (import "KERNEL", ver kexports.c) e de DLL.
 * k_* seguem o kernel: edi, esi, edx, ecx, r8d, r9d. As outras funcoes
 * deste compilador recebem os argumentos na pilha, de 4 em 4 bytes. */
#ifndef _SO_H
#define _SO_H

/* console */
void k_putc(int c);
void k_write(const char* s, int n);
/* Depuracao: escreve direto na serial (sem console/VGA). */
void k_dbg(const char* s, int n);
void k_puts(const char* s);
int k_getc(void);
/* Nao espera: -1 se nao ha tecla. Para jogos e lacos com k_ms(). */
int k_trygetc(void);
int k_readline(char* buf, int max);
void k_clear(void);
/* Cede o turno as outras tarefas do SO (WIN, HTTPD). Ponto de cooperacao
 * para threads de usuario e lacos de espera. */
void k_yield(void);

/* arquivos: "D:NOME.EXT", "NOME.EXT" ou pasta "D:\\temp\\net\\NOME.EXT" */
int k_fsize(const char* path);
int k_fload(const char* path, char* buf, int max);
/* Le len bytes a partir de offset. Devolve quantos leu, ou <0. */
int k_fread(const char* path, unsigned offset, char* buf, int len);
int k_fsave(const char* path, const char* data, int size);
int k_flist(int letter, char* out, int max);
/* Lista pasta: linhas "NOME" ou "NOME\\" (dir), separadas por \\n. <0 erro. */
int k_dirlist(const char* path, char* out, int max);
void k_pwd(char* out, int max);
int k_chdir(const char* path);
int k_drive(void);
int k_set_drive(int letter);
/* Unidade de onde o .EXE atual foi carregado ('C', 'D', ...; 0 = nenhuma). */
int k_exe_drive(void);

/* Uma entrada de pasta. date/time no formato FAT (0 = sem data):
 * dia = date & 31, mes = (date >> 5) & 15, ano = 1980 + (date >> 9);
 * hora = time >> 11, minuto = (time >> 5) & 63. */
struct k_dirent {
  char name[32];
  unsigned size;
  unsigned short date;
  unsigned short time;
  unsigned char dir;
  unsigned char pad[3];
};
/* Pasta "U:\\SRC": quantas entradas (sem . e ..); -1 se nao e pasta. */
int k_dir(const char* path, struct k_dirent* out, int max);
/* Como k_dir, com o nome longo inteiro (ate 255; k_dirent corta em 31 e
 * devolve o apelido 8.3). Kernels de 2026-11-20 em diante. */
struct k_direntl {
  char name[256];
  unsigned size;
  unsigned short date;
  unsigned short time;
  unsigned char dir;
  unsigned char pad[3];
};
int k_dir_long(const char* path, struct k_direntl* out, int max);
/* Ambiente do sistema (SET NOME=valor, o mesmo do prompt e do AUTOEXEC.BAT).
 * k_getenv copia o valor para out e devolve o tamanho; -1 se nao existe.
 * k_setenv com val vazio apaga. 0 ok. */
int k_getenv(const char* name, char* out, int max);
int k_setenv(const char* name, const char* val);
/* Apaga arquivo (aceita * e ?): 0, ou quantos com * e ?. -1 nao achou,
 * -3 e pasta, -4 somente leitura, -5 unidade sem apagar, -2 invalido. */
int k_fdelete(const char* path);
/* Renomeia/move no mesmo disco FAT sem copiar os dados. 0 ok, -1 nao existe,
 * -2 invalido/sem lugar, -3 destino e pasta, -5 unidade sem rename,
 * -6 outra unidade, -7 pasta para outra pasta (-5/-6/-7: copiar). */
int k_frename(const char* from, const char* to);
/* 0 ok; mkdir 1 = ja existe; rmdir -2 = nao vazia, -3 = nao e pasta. */
int k_mkdir(const char* path);
int k_rmdir(const char* path);
/* Espaco em KB de um disco FAT; -1 se nao for. */
int k_dfree(int letter, unsigned* total_kb, unsigned* free_kb);
/* Mascara das unidades montadas: bit 0 = A:, bit 1 = B:, ... */
unsigned k_drive_mask(void);

/* tela de texto 80x25: celulas caractere | cor << 8 (cor = fundo << 4 | frente).
 * -1 no WIN ou com a saida desviada: ai so ha k_putc. */
int k_vput(int row, int col, const unsigned short* cells, int n);
/* cursor na linha/coluna; row < 0 esconde */
void k_cursor(int row, int col);
/* Tamanho da tela de celulas (k_vput usa o passo cols). 80x25 no DOS; numa
 * janela do Command do WIN, depois desta chamada a grade segue o tamanho da
 * janela e cada mudanca chega a k_getc/k_trygetc como KEY_RESIZE.
 * 0 ok; -1 sem tela de celulas (preenche 80x25 mesmo assim). */
int k_textsize(int* cols, int* rows);
#ifndef KEY_RESIZE
#define KEY_RESIZE 0x1F0
#endif
/* Desenho do caractere ch na tela de texto: 16 bytes, um por linha, bit 7 a
 * esquerda. Vale para ch em toda a tela, inclusive o que ja esta escrito.
 * 0 ok; -1 ch invalido. k_font_set tambem da -1 no WIN ou com a saida desviada.
 * k_font_reset volta todos ao padrao; o kernel tambem faz isso quando o
 * programa que mudou a fonte termina. */
int k_font_get(int ch, unsigned char* glyph);
int k_font_set(int ch, const unsigned char* glyph);
void k_font_reset(void);
/* 0: o bit 7 da cor deixa de piscar e da fundo claro (cores 8-15). 1: volta a
 * piscar; o kernel tambem faz isso quando o programa termina. -1 no WIN. */
int k_blink(int on);

/* memoria do programa (liberada sozinha quando ele termina) */
void* k_malloc(unsigned n);
/* Kernel novo: endereco multiplo de align (potencia de 2). Programas que devem
 * rodar em kernel antigo procuram com k_dl_sym(0, "k_malloc_aligned", 0). */
void* k_malloc_aligned(unsigned n, unsigned align);
/* Kernel novo: excecoes da CPU do programa (#DE #UD #GP #PF #MF #AC #XM) vao
 * para entry em vez da tela de EXCEPTION. So para a libc (sigaction, x64):
 * procure com k_dl_sym(0, "k_fault_entry", 0). entry 0 desliga. */
int k_fault_entry(void* entry);
void k_free(void* p);
void* k_realloc(void* p, unsigned n);
unsigned k_msize(void* p);
void k_meminfo(unsigned* total, unsigned* freeb);

/* TOP: o que ocupa a memoria. Tamanhos em KB. */
#define KP_EXE 1   /* programa: pid > 0 */
#define KP_DLL 2   /* refs = quantos a usam */
#define KP_SYS 3   /* parte do kernel: KERNEL, BUFFERS, MEM>4GB */
#define KP_SVC 4   /* servico do kernel (REDE, HTTPD: refs = porta) */
struct k_proc {
  char name[32];
  unsigned pid;
  unsigned kind;
  unsigned state;     /* 'R' rodando, 'E' esperando outro, 'S' residente, 'A' ativo, 'P' parado */
  unsigned base;      /* endereco da imagem, 0 se nao ha */
  unsigned image_kb;  /* imagem (codigo, dados, bss, tabelas) */
  unsigned data_kb;   /* malloc e pilha; no KERNEL, o heap do kernel */
  unsigned refs;
  unsigned pad;
};
/* Devolve quantas entradas gravou em out. */
int k_ps(struct k_proc* out, int max);
/* Encerra a forca o programa pid (janelas e toda a memoria que reservou) ou,
 * com all=1, todos menos os de inicio e quem chamou (KILLALL). Devolve
 * quantos encerrou; -1 sem o WIN aberto, -2 o WIN nao respondeu.
 * Kernels de 2026-11-20 em diante. */
int k_kill(unsigned pid, int all);

/* Mapa da RAM fisica: 'cells' celulas de *cell_kb KB, do endereco 0 ao fim
 * da ultima regiao de RAM. kb[celula * KM_CATS + categoria] = KB daquela
 * categoria na celula. *cell_kb = 0 escolhe a menor potencia de 2 MB que
 * cabe. pid > 0 tambem soma em KM_DONO o que esse programa ocupa.
 * Devolve quantas celulas preencheu (no maximo 2048). */
#define KM_LIVRE 0
#define KM_KERNEL 1    /* imagem e heap do kernel, area baixa, boot */
#define KM_EXE 2
#define KM_DLL 3
#define KM_DADOS 4     /* malloc e pilha dos programas */
#define KM_BUFFERS 5   /* alocado pelo kernel na memoria alta */
#define KM_RESERV 6    /* nao e RAM: firmware, buracos, MMIO */
#define KM_DONO 7
#define KM_CATS 8
int k_memmap(unsigned* kb, int cells, unsigned* cell_kb, unsigned pid);

/* Placa de video e VRAM. Tamanhos em KB. Devolve 0. */
struct k_vram {
  char dev[24];        /* "Bochs VBE", "GOP (UEFI)", "VGA" */
  char drv[24];        /* driver da placa ativo; "" = desenho por CPU */
  unsigned pci_id;     /* vendor << 16 | device; 0 = nao achou */
  unsigned lfb;        /* endereco fisico do framebuffer */
  unsigned vram_kb;    /* memoria da placa; 0 = desconhecida */
  unsigned state;      /* 0 modo texto, 1 WIN na tela, 2 WIN atras de uma tela do DOS */
  unsigned w, h, bpp, pitch;   /* no modo texto: 80, 25, 4, 160 */
  unsigned front_kb;   /* VRAM: quadro visivel */
  unsigned page2_kb;   /* VRAM: segunda pagina (pagina dupla) */
  unsigned text_kb;    /* VRAM: tela de texto e fonte */
  unsigned back_kb;    /* RAM: back buffer */
  unsigned back_addr;
  unsigned wall_kb;    /* RAM: papel de parede escalado */
  unsigned win_kb;     /* RAM: superficies das janelas */
  unsigned win_n;
  unsigned gpu_kb;     /* memoria do driver da placa; 0 sem driver */
  unsigned pad[3];
};
int k_vram(struct k_vram* v);

/* CPUs (SMP): quantas o kernel acordou e o que cada uma esta fazendo.
 * Uso em milesimos: busy_ms / (busy_ms + idle_ms). index 0 = BSP. */
struct k_cpu_stat {
  unsigned id;        /* APIC id */
  unsigned index;     /* 0 = BSP */
  unsigned online;    /* 1 = a CPU subiu */
  unsigned state;     /* 0 = hlt/parada, 1 = ativa */
  unsigned busy_ms;   /* tempo trabalhando, desde o boot */
  unsigned idle_ms;   /* tempo parada (hlt), desde o boot */
  unsigned pad[2];
};
int k_cpu_count(void);
int k_cpu_stat(int i, struct k_cpu_stat* out);

/* Threads (SMP). Cada thread roda em qualquer CPU de cpu_mask (0 = todas),
 * preemptada pelo timer local. fn(arg) devolve o codigo do join.
 * stack_size 0 = 256 KB. A thread herda a base do FS (TLS) de quem cria.
 * O kernel tem um lock grande: k_* de varias threads sao serializados, o
 * codigo do programa roda em paralelo. exit()/k_exit numa thread encerra so
 * ela. tid > 0 ou erro < 0; o programa principal e o tid 0. */
int k_thread_create(int (*fn)(void*), void* arg, unsigned stack_size, unsigned cpu_mask);
int k_thread_join(int tid, int* code);
int k_thread_detach(int tid);
void k_thread_exit(int code);
int k_thread_self(void);
int k_thread_cpu(void);             /* CPU corrente (0 = BSP) */
int k_thread_affinity(int tid, unsigned mask);
void k_thread_yield(void);
int k_thread_tls(unsigned long long fs);
/* Futex: dorme enquanto *addr == val (timeout_ms < 0 = sem limite).
 * 0 = acordou, -11 = valor ja era outro, -110 = timeout. wake: n < 0 = todos. */
int k_futex_wait(volatile int* addr, int val, int timeout_ms);
int k_futex_wake(volatile int* addr, int n);

/* processos e DLLs */
void k_exit(int code);
int k_exec(const char* path, int argc, char** argv);
/* Uma linha do prompt: comando interno, .EXE ou .BAT. Nao grava historico.
 * A linha do chamador nao e alterada. Erro de carga vira 1. */
int k_shell(char* line);
/* k_shell para quem usa a libc (malloc, stdio): guarda o estado da LIBC.DLL
 * do chamador enquanto o comando roda, como system(). Esta na LIBC.DLL. */
int __so_shell(const char* line);
/* Como k_shell, mas a saida vai para out (com 0 no fim). *len = bytes
 * gravados, negativo se cortou em max-1. Devolve o codigo do comando. */
int k_shell_capture(char* line, char* out, int max, int* len);
const char* k_last_error(void);
int k_dll_load(const char* name);
void* k_dll_sym(int handle, const char* name);
void k_dll_free(int handle);

/* Bibliotecas ELF (.so). Programas usam dlopen/dlsym/dladdr (dlfcn.h); estas
 * chamadas sao a base deles na libc. Mesmos tipos de kernel/module.h. */
#define K_DL_NOLOAD 4       /* so devolve se ja estiver carregada */
#define K_DL_GLOBAL 0x100   /* simbolos visiveis as proximas cargas */
#define K_DL_EXE 1u          /* k_dl_info.flags: e o executavel */
#define K_DL_GLOBAL_SCOPE 2u /* k_dl_info.flags: esta no escopo global */

struct k_dl_info {
  unsigned long long base;          /* endereco de carga */
  unsigned long long size;
  const char* name;                 /* DT_SONAME, ou o nome do arquivo */
  const char* path;                 /* caminho completo no SODOS */
  unsigned long long phdr;          /* tabela de segmentos; 0 se nao foi mapeada */
  unsigned phnum;
  unsigned flags;
  unsigned long long dynamic;
  unsigned long long init, init_array, init_arraysz;
  unsigned long long fini, fini_array, fini_arraysz;
  unsigned long long tls_image, tls_filesz, tls_memsz, tls_align;
  unsigned long long tls_offset;    /* o bloco comeca em (ponteiro de thread - tls_offset) */
  unsigned tls_modid;               /* 0 = sem TLS */
  unsigned handle;
  unsigned long long tls_static;    /* so no executavel: bytes de TLS estatico por thread */
  unsigned long long stack_size;    /* so no executavel */
};

struct k_dl_addr {
  const char* fname;
  unsigned long long fbase;
  const char* sname;                /* 0 se nenhum simbolo cobre o endereco */
  unsigned long long saddr;
};

/* name 0 devolve o executavel. Handle > 0; 0 = falhou (k_last_error). */
int k_dl_open(const char* name, int flags);
/* handle 0 = escopo global; -1 = depois do modulo de caller (RTLD_NEXT). */
void* k_dl_sym(int handle, const char* name, const void* caller);
int k_dl_info(int handle, struct k_dl_info* out);
/* index-esimo modulo ELF do processo, em ordem de carga; 0 quando acaba. */
int k_dl_iter(int index, struct k_dl_info* out);
/* Proximo modulo a inicializar (dependencias primeiro) ou a finalizar (ordem
 * inversa); cada chamada marca o modulo. 0 quando nao ha mais. */
int k_dl_init_next(struct k_dl_info* out);
int k_dl_fini_next(struct k_dl_info* out);
int k_dl_addr(const void* addr, struct k_dl_addr* out);

/* tempo e aleatoriedade */
unsigned k_rdtsc(unsigned* hi);
unsigned k_ms(void);
/* data UTC AAAAMMDD, ou 0 se o relogio nao for confiavel */
int k_date(void);
/* hora UTC HHMMSS, ou -1 */
int k_time(void);
void k_random(unsigned char* out, int n);

/* rede: uma conexao TCP por vez */
int k_net_ready(void);
int k_dns(const char* host, unsigned* ip);
int k_tcp_connect(const char* host, int port);
int k_tcp_send(const void* data, int len);
int k_tcp_recv(void* data, int cap);
/* Bytes no buffer de recepcao, -1 se nao ha conexao (ou o par fechou). */
int k_net_pending(void);
void k_tcp_close(void);

/* rede: varios soquetes TCP nao bloqueantes (IP em ordem de host).
 * k_sock_connect: 1..64 ou < 0 (a conexao segue sozinha; veja k_sock_poll).
 * k_sock_poll: bits 1 ler, 2 escrever, 4 erro, 8 conectado.
 * k_sock_send: aceitos, -1 EAGAIN, -2 erro. k_sock_recv: lidos, 0 fim,
 * -1 EAGAIN, -2 erro (peek != 0 nao consome). k_sock_err: errno do Linux.
 * k_net_pump: le a placa e anda retransmissoes e filas. */
int k_sock_connect(unsigned ip, int port);
int k_sock_poll(int h);
int k_sock_err(int h);
int k_sock_send(int h, const void* data, int len);
int k_sock_recv(int h, void* data, int cap, int peek);
void k_sock_close(int h);
int k_sock_info(int h, unsigned* lip, int* lport, unsigned* rip, int* rport);
void k_net_pump(void);

/* janela canvas: so funciona num .EXE aberto pelo WIN. Cores 0xRRGGBB.
 * Use gui.h (GUI.DLL), que tem as constantes de eventos e teclas. */
int k_gui_open(const char* title);
/* abre com wpct% da largura e hpct% da altura da tela (0 = padrao) */
int k_gui_open_pct(const char* title, int wpct, int hpct);
void k_gui_close(void);
void k_gui_title(const char* title);
void k_gui_size(int* w, int* h);
void k_gui_font(int* cw, int* ch);
void k_gui_fill(int x, int y, int w, int h, unsigned rgb);
void k_gui_text(int x, int y, const char* s, int n, unsigned fg, unsigned bg, int flags);
void k_gui_present(void);
int k_gui_event(int* ev, int timeout_ms);
/* 1: a janela recebe o teclado em scancodes (GUI_EV_SCAN, apertar e soltar,
 * Ctrl/Alt/Shift inclusive) e todo movimento do rato (GUI_EV_MOVE) quando
 * tem o foco. Para emuladores (QEMU). Ctrl+F1..F12 trocam de tela e
 * Ctrl+Backspace fecha o WIN: continuam do kernel. */
int k_gui_raw(int on);
/* Teclado da janela: Ctrl+letra chega como 1..26 (Ctrl+C = 3). Ctrl+V o WIN
 * digita sozinho o texto da area de transferencia (k_clip_get); com
 * k_gui_clip(1) o programa recebe 22 e cola ele mesmo. */
int k_gui_clip(int on);
/* rgba = R,G,B,A por pixel. No modo retro A < 128 nao desenha; no modo
 * avancado o pixel e misturado com o que ja esta na janela. */
void k_gui_pixels(int x, int y, int w, int h, const void* rgba, int stride);

/* video: 0 = retro (alfa binario, baixo consumo), 1 = avancado (mistura).
 * k_gui_quality devolve o valor anterior; com mode < 0 so consulta. */
int k_gui_quality(int mode);
/* cor de fundo do desktop (0xRRGGBB); 0 volta ao padrao */
int k_win_bg(unsigned rgb);
/* papel de parede: rgba w*h*4 (R,G,B,A); rgba NULL limpa a imagem */
int k_win_wallpaper(const void* rgba, int w, int h);

/* interface (VIDEO.CFG): posicao do menu Iniciar (0 esq, 1 centro, 2 dir),
 * menu translucido (0/1) e painel de IA (0/1). Com valor < 0 so consulta. */
int k_ui_menu_pos(int pos);
int k_ui_glass(int on);
int k_ui_ia(int on);
/* Tema (Configuracoes > Temas): pastas em \SODOS\ICONS, escolha gravada em
 * D:\SODOS\TEMA.CFG pelo TEMAS.EXE. Os widgets (GUI.DLL) seguem sozinhos. */
typedef struct {
  unsigned gen;        /* muda a cada troca de tema */
  int modern;          /* 0 classico (relevo), 1 moderno (chapado, redondo) */
  int icons;           /* 1 = a pasta tem icones (.ICN) */
  char name[16];       /* pasta em \SODOS\ICONS */
  char dir[40];        /* "X:\SODOS\ICONS\NOME\" ou "" */
  unsigned face, bg, panel, text, text_dim, text_dis, sel_bg, sel_text, field_bg,
      border, accent;
  int radius, btn_h, row_h;   /* 0 = o padrao do GUI.DLL */
} k_ui_theme_t;
/* set: pasta do tema a carregar (NULL so consulta); out: estado atual.
 * Devolve gen (set NULL) ou 0 / -1 (sem a pasta). */
int k_ui_theme(const char* set, k_ui_theme_t* out);
/* Pede ao WIN que abra um programa (janela); 0 = pedido aceito. */
int k_win_launch(const char* path);
/* Alt+Tab (WINTAB.EXE). k_win_list: janelas da frente para tras, como estavam
 * ao apertar Alt+Tab (o proprio painel fica de fora). k_win_thumb: miniatura i
 * em w x h RGBA. k_win_shot: pixels da tela agora, RGBA. k_win_activate(id):
 * poe a janela na frente. k_alttab: 1 enquanto o Alt segue apertado; *tabs =
 * Tabs desde o Alt (Shift+Tab desconta). k_win_size: tamanho da tela.
 * k_gui_panel: janela sem titulo nem moldura no retangulo (x, y, w, h). */
typedef struct {
  int id;
  int x, y, w, h;
  int tw, th;
  char title[64];
} k_win_t;
int k_win_list(k_win_t* out, int max);
int k_win_thumb(int i, void* rgba, int w, int h);
int k_win_shot(int x, int y, int w, int h, void* rgba);
int k_win_activate(int id);
int k_alttab(int* tabs);
int k_win_size(int* w, int* h);
/* Tela principal (a primeira da tela estendida). */
int k_win_primary(int* w, int* h);
int k_gui_panel(const char* title, int x, int y, int w, int h);
/* Abre o arquivo com o programa associado a extensao (LINKEXT.DLL, SET
 * LINKEXT=1). 0 = abriu; 1 = sem associacao ou LINKEXT desligado. */
int k_open_file(const char* path);
/* Pendrive. op 0: letra ('U'...; 0 = SET PENDRIVE=0; -letra = nao montado);
 * 1: SCAN (texto em out, devolve a letra montada ou 0); 2: montar na letra
 * arg (0 ok, -1 sem pendrive, -2 sem FAT, -4 desligado, -5 letra ocupada,
 * -6 letra invalida); 3: EJECT (0 ok, -1 nao estava montado). */
int k_pendrive(int op, int arg, char* out, int max);
/* Area de transferencia do sistema (Ctrl+C/Ctrl+V do WIN e do DOS).
 * k_clip_set: n < 0 = ate o '\0'. k_clip_get devolve o tamanho copiado. */
void k_clip_set(const char* s, int n);
int k_clip_get(char* out, int max);

/* No cc, __export em uma funcao ou variavel global a coloca na tabela de
 * exports da DLL. No gcc (bootstrap) use link /export:nome ou /exportall. */
#ifdef __GNUC__
#define __export
#endif

#endif
