/*
 * ui/sodos.c - tela de texto do guest no console do SODOS (-display curses).
 *
 * O SODOS nao tem curses nem janela para o QEMU ainda. Este backend se
 * registra como o display "curses" e faz o mesmo papel: a VGA entrega a tela
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

/* Kernel do SODOS (c_compiler/include/so.h) */
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

static void sodos_refresh(DisplayChangeListener *d)
{
    static int64_t last_flush;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    if (now - last_flush >= 2000) {
        last_flush = now;
        bdrv_flush_all();
    }
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

static void sodos_atexit(void)
{
    k_blink(1);
    k_cursor(SO_ROWS - 1, 0);
}

static void sodos_display_init(DisplayState *ds, DisplayOptions *opts)
{
    (void)ds;
    (void)opts;
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
