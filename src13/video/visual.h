/* visual.h - Modulo grafico VGA */
#pragma once
#include <stdint.h>

void visual_run(void);
/* Desktop tipo Windows 3.1/95. ESC restaura modo texto 80x25. */
void win_run(void);
/* Carrega a fonte 8x16 CP850 (chamar no boot, ainda em modo texto). */
void vga_capture_bios_font(void);
/* Desenho em uso do caractere ch: 16 linhas, um byte por linha, bit 7 a esquerda. */
const uint8_t* vga_font_glyph(int ch);
int vga_font_get(int ch, uint8_t* glyph);
/* -1 se ch for invalido ou o modo grafico estiver na tela. */
int vga_font_set(int ch, const uint8_t* glyph);
/* Volta todos os caracteres ao desenho padrao. */
void vga_font_reset(void);
/* 0: bit 7 do atributo vira fundo claro em vez de piscar. 1: volta a piscar. */
void vga_text_blink(int on);
int vga_text_blink_get(void);
/* Forca modo texto 80x25 para que panic/exception seja visivel (ex.: em isr_dispatch). */
void vga_force_text_mode(void);

/* Janela "canvas" para programas .EXE rodando no WIN (exportada como k_gui_*).
 * Cores 0xRRGGBB; coordenadas relativas a area do cliente. */
#define GUI_EV_NONE  0
#define GUI_EV_DOWN  1  /* ev[1]=x ev[2]=y ev[3]=botao (1 esq, 2 dir) */
#define GUI_EV_KEY   2  /* ev[3]=tecla: ASCII ou GUI_KEY_* */
#define GUI_EV_CLOSE 3
#define GUI_EV_UP    4
#define GUI_EV_DRAG  5  /* movimento com o botao esquerdo apertado */
#define GUI_EV_WHEEL 6  /* roda do rato: ev[3] > 0 desce, < 0 sobe (passos) */
/* So com k_gui_raw(1) (emuladores): entrada crua da janela com foco. */
#define GUI_EV_SCAN  7  /* ev[3] = byte do teclado PS/2 (set 1, com E0 e soltar) */
#define GUI_EV_MOVE  8  /* ev[1]=x ev[2]=y ev[3]=botoes (1 esq, 2 dir, 4 meio) */
#define GUI_EV_RESIZE 9 /* a janela mudou de tamanho: ev[1]=largura ev[2]=altura */
#define GUI_KEY_UP    0x101
#define GUI_KEY_DOWN  0x102
#define GUI_KEY_LEFT  0x103
#define GUI_KEY_RIGHT 0x104
#define GUI_KEY_PGUP  0x105
#define GUI_KEY_PGDN  0x106
#define GUI_KEY_HOME  0x107
#define GUI_KEY_END   0x108
#define GUI_KEY_DEL   0x109
#define GUI_TEXT_NOBG     1
#define GUI_TEXT_BOLD     2
#define GUI_TEXT_UNDERLINE 4

int gui_open(const char* title);
void gui_close(void);
void gui_title(const char* title);
void gui_size(int* w, int* h);
void gui_font(int* cw, int* ch);
void gui_fill(int x, int y, int w, int h, uint32_t rgb);
void gui_text(int x, int y, const char* s, int n, uint32_t fg, uint32_t bg, int flags);
void gui_present(void);
int gui_event(int* ev, int timeout_ms);
/* 1 se a janela esta aberta e ha tecla, clique ou fechar na fila. */
int gui_input_pending(void);
/* Entrada crua (GUI_EV_SCAN/GUI_EV_MOVE) para a janela do programa: 0 ok. */
int gui_raw_set(int on);
/* 1: o programa trata Ctrl+V (recebe 22) em vez de o WIN digitar o texto. */
int gui_clip_set(int on);
void gui_pixels(int x, int y, int w, int h, const void* rgba, int stride);
/* Thread de programa (outra CPU) em volta de um gui_*: a janela corrente
 * passa a ser a do programa dela. enter devolve o que leave repoe (-1: nada
 * a repor). Fora de uma thread os dois nao fazem nada. */
int gui_thread_enter(void);
void gui_thread_leave(int save);
/* chamado pelo WIN em volta de mod_exec: so ai a janela pode abrir */
void gui_session(int allowed);
/* API HTTP: move o cursor. click 1 gera o aperto e o soltar do botao esquerdo. */
void mouse_api(int x, int y, int click);
void mouse_api_pos(int* x, int* y);
/* Comando MOUSE: move (move = 1) e poe na fila n estados dos botoes (bit 0
 * esquerdo, 1 direito, 2 meio), um por volta do laco do WIN: {1, 0} clica,
 * {2, 0} clica com o direito, {1} so aperta (arrastar). -1 = fila cheia.
 * mouse_api_busy: estados ainda na fila. */
int mouse_api_seq(int x, int y, int move, const uint8_t* btn, int n);
int mouse_api_busy(void);
/* 1 se o WIN e a tela que se ve agora (qualquer que seja a tela de quem
 * pergunta, ao contrario do win_screen_size). */
int win_shown_size(int* w, int* h);
/* COMMAND [linha]: Command novo no WIN rodando a linha. 0 ok, -1 sem WIN,
 * -2 outro pedido de abertura na fila. */
int win_command(const char* line);
/* 1 se o WIN (ou o modo grafico) esta na tela. */
int win_screen_size(int* w, int* h);
/* Tela principal (a primeira da tela estendida): SM_CXSCREEN do WINE. */
int win_primary_size(int* w, int* h);
/* Alt+Tab (WINTAB.EXE): janelas da frente para tras, tiradas ao apertar. */
struct k_win_info {
  int id;          /* para win_activate_api */
  int x, y, w, h;  /* na tela */
  int tw, th;      /* tamanho da miniatura guardada */
  char title[64];
};
int win_list_api(struct k_win_info* out, int max);
int win_thumb_api(int i, void* rgba, int w, int h);
int win_shot_api(int x, int y, int w, int h, void* rgba);
int win_activate_api(int id);
/* 1 enquanto o Alt do Alt+Tab continua apertado; tabs = Tabs ate agora. */
int alttab_api(int* tabs);
int alttab_wait_api(int seq, int ms);
/* Janela sem titulo nem moldura no retangulo (painel do WINTAB). */
int gui_panel_open(const char* title, int x, int y, int w, int h);
/* Injeta uma tecla ASCII na janela GUI com foco (automacao/testes). */
int win_api_key(int ch);
/* Linha y em BGR. Devolve a largura, ou -1. */
int win_screen_bgr(int y, uint8_t* dst, int max_px);
/* Configuracao de video (VIDEOCFG.EXE): 0 = retro, 1 = avancado. */
int gui_quality_get(void);
int gui_quality_set(int mode);
/* Cor de fundo do desktop (0xRRGGBB; 0 volta ao teal padrao). */
int win_bg_set(uint32_t rgb);
/* Papel de parede: rgba w*h*4; NULL/0 limpa. */
int win_wall_set(const uint8_t* rgba, int w, int h);
/* interface: posicao do menu (0 esq, 1 centro, 2 dir), menu translucido,
 * painel de IA ligado. Os _get devolvem o valor atual. */
int ui_menu_pos_set(int pos);
int ui_glass_set(int on);
int ui_ia_set(int on);
int ui_menu_pos_get(void);
int ui_glass_get(void);
int ui_ia_get(void);
/* Tema da interface (Configuracoes > Temas). A mesma estrutura do so.h
 * (k_ui_theme_t): campos novos so no fim. */
struct ui_theme_info {
  unsigned gen;        /* muda a cada troca de tema */
  int modern;          /* 0 classico (relevo), 1 moderno (chapado, redondo) */
  int icons;           /* 1 = a pasta tem icones (.ICN) */
  char name[16];       /* pasta em \SODOS\ICONS */
  char dir[40];        /* "X:\SODOS\ICONS\NOME\" ou "" */
  unsigned face, bg, panel, text, text_dim, text_dis, sel_bg, sel_text, field_bg,
      border, accent;
  int radius, btn_h, row_h;   /* 0 = o padrao do GUI.DLL */
};
/* set != NULL: carrega o tema (0 ok, -1 sem a pasta). out != NULL: copia o
 * atual. Devolve gen (set NULL) ou o resultado da troca. */
int ui_theme_api(const char* set, struct ui_theme_info* out);
/* abre a janela canvas docada na lateral direita (painel de IA). */
int gui_dock_open(const char* title, int w);
int gui_open_pct(const char* title, int wpct, int hpct);
/* Pede ao WIN que abra um programa (janela). Executa no proximo quadro. */
int win_launch(const char* path);
/* Arquivo/pasta mudou: se o caminho e da pasta DESKTOP, o desktop rele. */
void win_desk_touch(const char* path);

/* Lista "titulo x y w h". -1 se o desktop nao esta aberto. */
int win_windows(char* out, int max);
/* Fecha a janela com esse titulo. 0 = fechou. */
int win_close(const char* name);

/* TOP: placa de video, VRAM e o que o modo grafico ocupa (k_vram).
 * Mesmo layout de struct k_vram em c_compiler/include/so.h. Tamanhos em KB. */
struct k_vram {
  char dev[24];        /* "Bochs VBE", "GOP (UEFI)", "VGA" */
  char drv[24];        /* driver da placa ativo; "" = desenho por CPU */
  uint32_t pci_id;     /* vendor << 16 | device; 0 = nao achou */
  uint32_t lfb;        /* endereco fisico do framebuffer (BAR0) */
  uint32_t vram_kb;    /* memoria da placa; 0 = desconhecida */
  uint32_t state;      /* 0 modo texto, 1 WIN na tela, 2 WIN atras de uma tela do DOS */
  uint32_t w, h, bpp, pitch;   /* no modo texto: 80, 25, 4, 160 */
  /* na VRAM */
  uint32_t front_kb;   /* quadro visivel */
  uint32_t page2_kb;   /* segunda pagina (pagina dupla Bochs) */
  uint32_t text_kb;    /* tela de texto e fonte VGA */
  /* na RAM, por causa do modo grafico */
  uint32_t back_kb;    /* back buffer */
  uint32_t back_addr;
  uint32_t wall_kb;    /* papel de parede ja escalado */
  uint32_t win_kb;     /* superficies das janelas dos programas */
  uint32_t win_n;
  uint32_t gpu_kb;     /* memoria do driver da placa (docs/GPU.md); 0 sem driver */
  uint32_t pad[3];
};
int win_vram_info(struct k_vram* v);

/* Encerra a forca o programa pid (com a janela e a memoria) ou, com all=1,
 * todos menos os de inicio e quem pediu. Quantas tarefas; -1 sem WIN. */
int win_kill(uint32_t pid, int all);
int win_open(void);   /* 1 se o WIN esta aberto (na tela ou ao fundo) */
int win_task_pids(const char* name, uint32_t* out, int max);
int win_task_list(char (*names)[32], uint32_t* pids, int max);
