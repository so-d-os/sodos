/*
 * ui/sodos.c - tela do guest no SODOS (-display curses).
 *
 * Dois modos, escolhidos na partida:
 *
 * Janela do WIN (QEMU aberto pelo WIN: k_gui_open da certo). A superficie da
 * VGA (texto ou grafico, 32 bpp) vai para a janela com k_gui_pixels,
 * escalada se nao couber. A janela pede entrada crua (k_gui_raw): cada byte
 * do teclado PS/2 do SODOS vai igual para o i8042 do guest (apertar e soltar,
 * Ctrl/Alt/Shift de verdade) e o movimento do rato vira deltas do rato PS/2.
 * Fechar a janela desliga o QEMU.
 *
 * Console de texto (fora do WIN). O display "curses" de verdade: a VGA entrega a tela
 * de texto em celulas (graphic_hw_text_update) e cada linha alterada vai para
 * a tela de texto 80x25 do SODOS com k_vput, no mesmo formato da VGA
 * (caractere | cor << 8). Modos graficos nao aparecem.
 *
 * Teclado: k_trygetc devolve caracteres e teclas especiais, sem estado de
 * Ctrl/Alt e sem soltar tecla. Cada tecla vira aperta+solta no i8042 do
 * guest (layout US). Para Ctrl e Alt: F11 aplica Ctrl e F12 aplica Alt a
 * proxima tecla (F12 F = Alt+F, que abre o menu Arquivo do EDIT). F12 F12
 * desliga o QEMU gravando os discos; Ctrl+C do SODOS mata o QEMU na hora.
 *
 * Discos: a posix-lite guarda o arquivo da imagem em memoria e so grava no
 * fsync/close. A cada 2 s o refresh pede bdrv_flush_all (so o que mudou),
 * entao um Ctrl+C perde no maximo os ultimos 2 s de escrita do guest.
 *
 * Copiado para vendor/qemu-x/ui/ por scripts/configure-qemu.sh; ligado no
 * build pelo patches/0003-sodos-display.patch.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "ui/input.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "block/block.h"
#include "qemu/timer.h"

/* Kernel do SODOS (c_compiler/include/so.h, gui.h) */
int k_gui_open(const char *title);
void k_gui_size(int *w, int *h);
void k_gui_present(void);
int k_gui_event(int *ev, int timeout_ms);
void k_gui_pixels(int x, int y, int w, int h, const void *rgba, int stride);
int k_gui_raw(int on);
#define SO_GUI_EV_CLOSE 3
#define SO_GUI_EV_SCAN  7
#define SO_GUI_EV_MOVE  8
int k_vput(int row, int col, const unsigned short *cells, int n);
void k_cursor(int row, int col);
void k_clear(void);
int k_blink(int on);
int k_trygetc(void);

#define SO_COLS 80
#define SO_ROWS 25

static DisplayChangeListener *dcl;
static console_ch_t *screen;
static int width = SO_COLS, height = SO_ROWS;
static int invalidate;
static int next_mod;            /* 0, 1 = Ctrl, 2 = Alt (F11/F12) */

/*
 * A VGA guarda a celula como o curses quer (hw/display/vga.c, VMEM2CHTYPE):
 * frente nos bits 8-10, intensidade da frente no bit 21, fundo nos bits
 * 11-13. Volta ao atributo VGA (fundo << 4 | frente).
 */
static unsigned short cell_of(console_ch_t c)
{
    unsigned ch = c & 0xff;
    unsigned fg = ((c >> 8) & 7) | (((c >> 21) & 1) << 3);
    unsigned bg = (c >> 11) & 7;
    return (unsigned short)(ch | ((bg << 4 | fg) << 8));
}

static void sodos_update(DisplayChangeListener *d, int x, int y, int w, int h)
{
    unsigned short line[SO_COLS];
    int r, i, n;

    (void)d;
    (void)x;
    (void)w;
    n = width < SO_COLS ? width : SO_COLS;
    for (r = y; r < y + h && r < SO_ROWS; r++) {
        for (i = 0; i < n; i++) {
            line[i] = cell_of(screen[r * width + i]);
        }
        k_vput(r, 0, line, n);
    }
}

static void sodos_resize(DisplayChangeListener *d, int w, int h)
{
    (void)d;
    if (w > 0 && h > 0 && w * h <= 160 * 100) {
        width = w;
        height = h;
    }
    invalidate = 1;
}

static void sodos_cursor(DisplayChangeListener *d, int x, int y)
{
    (void)d;
    if (x >= 0 && y >= 0 && x < SO_COLS && y < SO_ROWS) {
        k_cursor(y, x);
    } else {
        k_cursor(-1, 0);
    }
}

/* ---------- teclado ---------- */

#define SH 0x100                /* com Shift */

static int ascii_qcode(int c)
{
    static const char low[] = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
    static const char up[]  = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
    static const QKeyCode qc[] = {
        Q_KEY_CODE_1, Q_KEY_CODE_2, Q_KEY_CODE_3, Q_KEY_CODE_4, Q_KEY_CODE_5,
        Q_KEY_CODE_6, Q_KEY_CODE_7, Q_KEY_CODE_8, Q_KEY_CODE_9, Q_KEY_CODE_0,
        Q_KEY_CODE_MINUS, Q_KEY_CODE_EQUAL,
        Q_KEY_CODE_Q, Q_KEY_CODE_W, Q_KEY_CODE_E, Q_KEY_CODE_R, Q_KEY_CODE_T,
        Q_KEY_CODE_Y, Q_KEY_CODE_U, Q_KEY_CODE_I, Q_KEY_CODE_O, Q_KEY_CODE_P,
        Q_KEY_CODE_BRACKET_LEFT, Q_KEY_CODE_BRACKET_RIGHT,
        Q_KEY_CODE_A, Q_KEY_CODE_S, Q_KEY_CODE_D, Q_KEY_CODE_F, Q_KEY_CODE_G,
        Q_KEY_CODE_H, Q_KEY_CODE_J, Q_KEY_CODE_K, Q_KEY_CODE_L,
        Q_KEY_CODE_SEMICOLON, Q_KEY_CODE_APOSTROPHE, Q_KEY_CODE_GRAVE_ACCENT,
        Q_KEY_CODE_BACKSLASH,
        Q_KEY_CODE_Z, Q_KEY_CODE_X, Q_KEY_CODE_C, Q_KEY_CODE_V, Q_KEY_CODE_B,
        Q_KEY_CODE_N, Q_KEY_CODE_M, Q_KEY_CODE_COMMA, Q_KEY_CODE_DOT,
        Q_KEY_CODE_SLASH,
    };
    const char *p;

    switch (c) {
    case ' ':  return Q_KEY_CODE_SPC;
    case '\n':
    case '\r': return Q_KEY_CODE_RET;
    case '\b':
    case 127:  return Q_KEY_CODE_BACKSPACE;
    case '\t': return Q_KEY_CODE_TAB;
    case 27:   return Q_KEY_CODE_ESC;
    }
    if (c <= 0 || c > 127) {
        return -1;
    }
    p = strchr(low, c);
    if (p) {
        return qc[p - low];
    }
    p = strchr(up, c);
    if (p) {
        return qc[p - up] | SH;
    }
    return -1;
}

/* Teclas especiais do SODOS (shell.h: KEY_UP = 0x100 ..., KEY_F(n) = 0x140 + n) */
static int special_qcode(int c)
{
    static const QKeyCode nav[] = {
        Q_KEY_CODE_UP, Q_KEY_CODE_DOWN, Q_KEY_CODE_LEFT, Q_KEY_CODE_RIGHT,
        Q_KEY_CODE_PGUP, Q_KEY_CODE_PGDN, Q_KEY_CODE_HOME, Q_KEY_CODE_END,
        Q_KEY_CODE_DELETE, Q_KEY_CODE_INSERT,
    };
    static const QKeyCode fn[] = {
        Q_KEY_CODE_F1, Q_KEY_CODE_F2, Q_KEY_CODE_F3, Q_KEY_CODE_F4,
        Q_KEY_CODE_F5, Q_KEY_CODE_F6, Q_KEY_CODE_F7, Q_KEY_CODE_F8,
        Q_KEY_CODE_F9, Q_KEY_CODE_F10, Q_KEY_CODE_F11, Q_KEY_CODE_F12,
    };
    if (c >= 0x100 && c <= 0x109) {
        return nav[c - 0x100];
    }
    if (c >= 0x141 && c <= 0x14c) {
        return fn[c - 0x141];
    }
    return -1;
}

static void tap(QKeyCode q, int shift, int mod)
{
    QemuConsole *con = dcl->con;
    QKeyCode m = mod == 1 ? Q_KEY_CODE_CTRL : Q_KEY_CODE_ALT;

    if (mod) {
        qemu_input_event_send_key_qcode(con, m, true);
        qemu_input_event_send_key_delay(0);
    }
    if (shift) {
        qemu_input_event_send_key_qcode(con, Q_KEY_CODE_SHIFT, true);
        qemu_input_event_send_key_delay(0);
    }
    qemu_input_event_send_key_qcode(con, q, true);
    qemu_input_event_send_key_delay(0);
    qemu_input_event_send_key_qcode(con, q, false);
    qemu_input_event_send_key_delay(0);
    if (shift) {
        qemu_input_event_send_key_qcode(con, Q_KEY_CODE_SHIFT, false);
        qemu_input_event_send_key_delay(0);
    }
    if (mod) {
        qemu_input_event_send_key_qcode(con, m, false);
        qemu_input_event_send_key_delay(0);
    }
}

static void sodos_keys(void)
{
    int c;

    while ((c = k_trygetc()) > 0) {
        int q;
        if (c == 0x14b) {           /* F11: Ctrl na proxima tecla */
            next_mod = 1;
            continue;
        }
        if (c == 0x14c) {           /* F12: Alt na proxima tecla */
            if (next_mod == 2) {    /* F12 F12: desliga gravando os discos */
                next_mod = 0;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
                continue;
            }
            next_mod = 2;
            continue;
        }
        q = c >= 0x100 ? special_qcode(c) : ascii_qcode(c);
        if (q < 0) {
            continue;
        }
        /* Ctrl/Alt + letra vai sem Shift (Alt+F, nao Alt+Shift+F) */
        tap(q & 0xff, next_mod ? 0 : (q & SH) != 0, next_mod);
        next_mod = 0;
    }
}

static void flush_disks(void)
{
    static int64_t last_flush;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    if (now - last_flush >= 2000) {
        last_flush = now;
        bdrv_flush_all();
    }
}

static void sodos_refresh(DisplayChangeListener *d)
{
    flush_disks();
    if (invalidate) {
        k_clear();
        graphic_hw_invalidate(d->con);
        invalidate = 0;
    }
    graphic_hw_text_update(d->con, screen);
    sodos_keys();
}

/* Troca de modo de video (texto <-> grafico, 80x25 <-> 80x50): redesenha. */
static void sodos_gfx_switch(DisplayChangeListener *d, DisplaySurface *s)
{
    (void)d;
    (void)s;
    invalidate = 1;
}

static bool sodos_check_format(DisplayChangeListener *d, pixman_format_code_t f)
{
    (void)d;
    (void)f;
    return true;
}

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name           = "sodos",
    .dpy_text_update    = sodos_update,
    .dpy_text_resize    = sodos_resize,
    .dpy_text_cursor    = sodos_cursor,
    .dpy_refresh        = sodos_refresh,
    .dpy_gfx_switch     = sodos_gfx_switch,
    .dpy_gfx_check_format = sodos_check_format,
};

/* ---------- janela do WIN ---------- */

static int win_w, win_h;            /* area da janela */
static uint8_t *win_rgba;           /* win_w * win_h * 4 (R,G,B,A) */
static DisplaySurface *gsurf;
static int gw, gh;                  /* tamanho do guest */
static int dw, dh, dx0, dy0;        /* area desenhada (escalada) e margem */
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static int kb_e0;

/*
 * Rato PS/2 relativo com o cursor do guest preso ao do WIN. O X acelera
 * movimentos acima de ~4 px por pacote (limiar padrao), o que afastaria os
 * dois cursores. Entao: no comeco (e sempre que o alvo encosta na borda de
 * cima/esquerda) empurra o cursor do guest ate o canto, onde ele para, e dai
 * em diante anda em passos de no maximo 4 px, um pacote cada, ate o alvo.
 * Botoes so mudam quando o cursor chegou: o clique cai onde foi feito.
 */
#define M_STEP 4                /* px por pacote, abaixo do limiar do X */
#define M_BUDGET 48             /* pacotes por refresh (fila PS/2 de 256 bytes) */
#define M_Q 32
static int m_cur_x, m_cur_y;    /* onde o cursor do guest esta */
static int m_homed;
static uint32_t m_btn;
static int m_qx[M_Q], m_qy[M_Q], m_qn;
static uint32_t m_qb[M_Q];

static void dirty_add(int x0, int y0, int x1, int y1)
{
    if (dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0) {
        dirty_x0 = x0; dirty_y0 = y0; dirty_x1 = x1; dirty_y1 = y1;
        return;
    }
    dirty_x0 = MIN(dirty_x0, x0);
    dirty_y0 = MIN(dirty_y0, y0);
    dirty_x1 = MAX(dirty_x1, x1);
    dirty_y1 = MAX(dirty_y1, y1);
}

/* Guest (gx, gy, w, h) -> janela, escalando por vizinho mais proximo. */
static void win_blit(int x, int y, int w, int h)
{
    const uint8_t *src;
    int stride, ax, bx, ay, by, i, j;

    if (!gsurf || !win_rgba || gw <= 0 || gh <= 0 ||
        surface_bits_per_pixel(gsurf) != 32) {
        return;
    }
    src = surface_data(gsurf);
    stride = surface_stride(gsurf);
    ax = x * dw / gw;
    bx = ((x + w) * dw + gw - 1) / gw;
    ay = y * dh / gh;
    by = ((y + h) * dh + gh - 1) / gh;
    bx = MIN(bx, dw);
    by = MIN(by, dh);
    for (j = ay; j < by; j++) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)(j * gh / dh) * stride);
        uint8_t *dst = win_rgba + ((size_t)(dy0 + j) * win_w + dx0 + ax) * 4;
        for (i = ax; i < bx; i++, dst += 4) {
            uint32_t p = row[dw == gw ? i : i * gw / dw];
            dst[0] = p >> 16;
            dst[1] = p >> 8;
            dst[2] = p;
            dst[3] = 255;
        }
    }
    dirty_add(dx0 + ax, dy0 + ay, dx0 + bx, dy0 + by);
}

static void win_layout(void)
{
    size_t k;

    gw = gsurf ? surface_width(gsurf) : 0;
    gh = gsurf ? surface_height(gsurf) : 0;
    if (gw <= 0 || gh <= 0) {
        return;
    }
    if (gw <= win_w && gh <= win_h) {
        /* cabe: amplia por um fator inteiro (720x400 do DOS vira 1440x800) */
        int f = MIN(win_w / gw, win_h / gh);
        dw = gw * f;
        dh = gh * f;
    } else if ((int64_t)gw * win_h > (int64_t)gh * win_w) {
        dw = win_w;
        dh = (int)((int64_t)gh * win_w / gw);
    } else {
        dh = win_h;
        dw = (int)((int64_t)gw * win_h / gh);
    }
    dx0 = (win_w - dw) / 2;
    dy0 = (win_h - dh) / 2;
    for (k = 0; k < (size_t)win_w * win_h; k++) {     /* margem preta */
        win_rgba[k * 4 + 0] = win_rgba[k * 4 + 1] = win_rgba[k * 4 + 2] = 0;
        win_rgba[k * 4 + 3] = 255;
    }
    dirty_add(0, 0, win_w, win_h);
    win_blit(0, 0, gw, gh);
}

static void win_gfx_update(DisplayChangeListener *d, int x, int y, int w, int h)
{
    (void)d;
    win_blit(x, y, w, h);
}

static void win_gfx_switch(DisplayChangeListener *d, DisplaySurface *s)
{
    (void)d;
    gsurf = s;
    win_layout();
}

/* So 32 bpp: a VGA converte as outras profundidades para nos. */
static bool win_check_format(DisplayChangeListener *d, pixman_format_code_t f)
{
    (void)d;
    return f == PIXMAN_x8r8g8b8 || f == PIXMAN_a8r8g8b8;
}

/* Evento do WIN: alvo (em coordenadas do guest) e botoes, na fila. */
static void win_mouse(int wx, int wy, uint32_t btn)
{
    int x, y;

    if (dw <= 0 || dh <= 0) {
        return;
    }
    x = MAX(0, MIN(gw - 1, (wx - dx0) * gw / dw));
    y = MAX(0, MIN(gh - 1, (wy - dy0) * gh / dh));
    if ((x == 0 || y == 0) && !btn) {
        m_homed = 0;            /* encostou na borda: reancora no canto */
    }
    if (m_qn > 0 && m_qb[m_qn - 1] == btn) {        /* so moveu: junta */
        m_qx[m_qn - 1] = x;
        m_qy[m_qn - 1] = y;
        return;
    }
    if (m_qn == M_Q) {                              /* cheia: perde o mais velho */
        memmove(m_qx, m_qx + 1, sizeof(int) * (M_Q - 1));
        memmove(m_qy, m_qy + 1, sizeof(int) * (M_Q - 1));
        memmove(m_qb, m_qb + 1, sizeof(uint32_t) * (M_Q - 1));
        m_qn--;
    }
    m_qx[m_qn] = x;
    m_qy[m_qn] = y;
    m_qb[m_qn] = btn;
    m_qn++;
}

static void mouse_rel(QemuConsole *con, int dx, int dy)
{
    qemu_input_queue_rel(con, INPUT_AXIS_X, dx);
    qemu_input_queue_rel(con, INPUT_AXIS_Y, dy);
    qemu_input_event_sync();
}

/* Anda com o cursor do guest ate o primeiro alvo da fila. */
static void win_mouse_drain(QemuConsole *con)
{
    static uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT] = 1, [INPUT_BUTTON_RIGHT] = 2, [INPUT_BUTTON_MIDDLE] = 4,
    };
    int budget = M_BUDGET;

    while (m_qn > 0 && budget > 0) {
        int tx = m_qx[0], ty = m_qy[0];
        if (!m_homed) {
            /* ao canto: o X prende o cursor em (0,0) */
            mouse_rel(con, -(gw + gh), -(gw + gh));
            m_cur_x = m_cur_y = 0;
            m_homed = 1;
            budget -= 8;
            if (tx == 0 && ty == 0) {
                goto arrived;
            }
        }
        if (m_cur_x != tx || m_cur_y != ty) {
            int sx = MAX(-M_STEP, MIN(M_STEP, tx - m_cur_x));
            int sy = MAX(-M_STEP, MIN(M_STEP, ty - m_cur_y));
            mouse_rel(con, sx, sy);
            m_cur_x += sx;
            m_cur_y += sy;
            budget--;
            continue;
        }
arrived:
        if (m_qb[0] != m_btn) {
            qemu_input_update_buttons(con, bmap, m_btn, m_qb[0]);
            qemu_input_event_sync();
            m_btn = m_qb[0];
            budget--;
        }
        {
            memmove(m_qx, m_qx + 1, sizeof(int) * (m_qn - 1));
            memmove(m_qy, m_qy + 1, sizeof(int) * (m_qn - 1));
            memmove(m_qb, m_qb + 1, sizeof(uint32_t) * (m_qn - 1));
            m_qn--;
        }
    }
}

static void win_events(QemuConsole *con)
{
    int ev[4], t, n = 0;

    while (n++ < 256 && (t = k_gui_event(ev, 0)) > 0) {
        if (t == SO_GUI_EV_SCAN) {
            int b = ev[3] & 0xff;
            if (b == 0xe0) {
                kb_e0 = 1;
                continue;
            }
            if (b == 0xe1) {            /* Pause: ignora a sequencia */
                continue;
            }
            qemu_input_event_send_key_number(con, (b & 0x7f) | (kb_e0 ? 0x80 : 0),
                                             !(b & 0x80));
            /* Fila do QEMU: um byte por vez para o guest, como no curses
             * (sem isso o DOS perdia o Alt de Alt+F). */
            qemu_input_event_send_key_delay(0);
            kb_e0 = 0;
        } else if (t == SO_GUI_EV_MOVE) {
            win_mouse(ev[1], ev[2], (uint32_t)ev[3] & 7);
        } else if (t == SO_GUI_EV_CLOSE) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
        }
    }
}

static void win_refresh(DisplayChangeListener *d)
{
    flush_disks();
    graphic_hw_update(d->con);
    if (dirty_x1 > dirty_x0 && dirty_y1 > dirty_y0) {
        k_gui_pixels(dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0,
                     win_rgba + ((size_t)dirty_y0 * win_w + dirty_x0) * 4, win_w * 4);
        k_gui_present();
        dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;
    }
    /* k_gui_event tambem devolve a vez ao WIN (cursor, outras janelas) */
    win_events(d->con);
    win_mouse_drain(d->con);
}

static const DisplayChangeListenerOps win_ops = {
    .dpy_name             = "sodos-win",
    .dpy_gfx_update       = win_gfx_update,
    .dpy_gfx_switch       = win_gfx_switch,
    .dpy_gfx_check_format = win_check_format,
    .dpy_refresh          = win_refresh,
};

/* 0 se abriu a janela do WIN. */
static int win_init(void)
{
    if (k_gui_open("QEMU") != 0) {
        return -1;
    }
    k_gui_size(&win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) {
        return -1;
    }
    win_rgba = g_malloc0((size_t)win_w * win_h * 4);
    k_gui_raw(1);
    dcl = g_new0(DisplayChangeListener, 1);
    dcl->con = qemu_console_lookup_default();
    dcl->ops = &win_ops;
    register_displaychangelistener(dcl);
    return 0;
}

static void sodos_atexit(void)
{
    k_blink(1);
    k_cursor(SO_ROWS - 1, 0);
}

static void sodos_display_init(DisplayState *ds, DisplayOptions *opts)
{
    (void)ds;
    (void)opts;
    if (win_init() == 0) {
        return;
    }
    screen = g_new0(console_ch_t, 160 * 100);
    k_blink(0);                 /* bit 7 da cor = fundo claro, como no DOS */
    k_clear();
    atexit(sodos_atexit);

    dcl = g_new0(DisplayChangeListener, 1);
    dcl->con = qemu_console_lookup_default();
    dcl->ops = &dcl_ops;
    register_displaychangelistener(dcl);
    invalidate = 1;
}

static QemuDisplay qemu_display_sodos = {
    .type       = DISPLAY_TYPE_CURSES,
    .init       = sodos_display_init,
};

static void register_sodos(void)
{
    qemu_display_register(&qemu_display_sodos);
}

type_init(register_sodos);
