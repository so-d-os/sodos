/* Tetris para o WIN. Subconjunto C++ aceito pelo compilador do SODOS. */
#include <gui.h>
#include <stdio.h>
#include <string.h>

/* Matrizes 4x4; a rotacao e calculada sem alocacao dinamica. */
static unsigned shapes[7] = {0x00F0, 0x0066, 0x0072, 0x0036,
                             0x0063, 0x0071, 0x0074};
static unsigned colors[8] = {0x17202C, 0x35D9ED, 0xF4D34E, 0xB77DF2,
                             0x64D873, 0xF16C73, 0x5C8FED, 0xF0A34A};

class Tetris {
public:
  int board[200];
  int piece, next, rotation, x, y, score, lines, paused, over;
  unsigned random;

  int cell(int p, int r, int cx, int cy) {
    int i, t;
    /* O nao muda de posicao ao girar. */
    if (p == 1) r = 0;
    for (i = 0; i < r; i++) {
      t = cx; cx = cy; cy = 3 - t;
    }
    return (shapes[p] >> (cy * 4 + cx)) & 1;
  }

  int pick() {
    random = random * 1664525u + 1013904223u;
    return (int)((random >> 16) % 7);
  }

  int fits(int nx, int ny, int nr) {
    int a, b, bx, by;
    for (b = 0; b < 4; b++) for (a = 0; a < 4; a++) {
      if (cell(piece, nr, a, b)) {
        bx = nx + a; by = ny + b;
        if (bx < 0 || bx >= 10 || by < 0 || by >= 20) return 0;
        if (board[by * 10 + bx]) return 0;
      }
    }
    return 1;
  }

  void spawn() {
    piece = next; next = pick(); rotation = 0; x = 3; y = 0;
    if (!fits(x, y, rotation)) over = 1;
  }

  void reset() {
    memset(board, 0, sizeof(board));
    score = 0; lines = 0; paused = 0; over = 0;
    next = pick(); spawn();
  }

  int interval() {
    int ms = 700 - (lines / 10) * 50;
    if (ms < 100) ms = 100;
    return ms;
  }

  void lock() {
    int a, b, row, col, dst, full, cleared;
    for (b = 0; b < 4; b++) for (a = 0; a < 4; a++)
      if (cell(piece, rotation, a, b))
        board[(y + b) * 10 + x + a] = piece + 1;
    dst = 19; cleared = 0;
    for (row = 19; row >= 0; row--) {
      full = 1;
      for (col = 0; col < 10; col++)
        if (!board[row * 10 + col]) full = 0;
      if (full) cleared++;
      else {
        for (col = 0; col < 10; col++)
          board[dst * 10 + col] = board[row * 10 + col];
        dst--;
      }
    }
    while (dst >= 0) {
      for (col = 0; col < 10; col++) board[dst * 10 + col] = 0;
      dst--;
    }
    /* 100, 300, 500, 800 por uma, duas, tres ou quatro linhas. */
    if (cleared) {
      int points = cleared * 200 - 100;
      if (cleared == 4) points = 800;
      score += points * (lines / 10 + 1);
      lines += cleared;
    }
    spawn();
  }

  int down() {
    if (fits(x, y + 1, rotation)) { y++; return 1; }
    lock(); return 0;
  }

  void turn() {
    int r = (rotation + 1) % 4;
    if (fits(x, y, r)) rotation = r;
    else if (fits(x - 1, y, r)) { x--; rotation = r; }
    else if (fits(x + 1, y, r)) { x++; rotation = r; }
    else if (fits(x - 2, y, r)) { x -= 2; rotation = r; }
    else if (fits(x + 2, y, r)) { x += 2; rotation = r; }
  }
};

static Tetris game;

static void block(int x, int y, int size, int color) {
  gui_fill(x, y, size - 1, size - 1, colors[color]);
  if (color) gui_frame(x, y, size - 1, size - 1, 0xDDE5EF);
}

static void label(int x, int y, const char* text) {
  gui_text(x, y, text, -1, 0xE5EDF5, 0x101722, 0);
}

static void draw() {
  int w = gui_width(), h = gui_height();
  int size = (h - 32) / 20;
  int a, b, panel;
  char text[80];
  if (size > (w - 200) / 10) size = (w - 200) / 10;
  gui_fill(0, 0, w, h, 0x101722);
  if (size < 6) {
    label(8, 8, "Aumente a janela para jogar.");
    gui_present(); return;
  }
  panel = 24 + size * 10;
  for (b = 0; b < 20; b++) for (a = 0; a < 10; a++)
    block(12 + a * size, 16 + b * size, size, game.board[b * 10 + a]);
  if (!game.over) {
    for (b = 0; b < 4; b++) for (a = 0; a < 4; a++)
      if (game.cell(game.piece, game.rotation, a, b))
        block(12 + (game.x + a) * size, 16 + (game.y + b) * size,
              size, game.piece + 1);
  }
  label(panel, 16, "TETRIS");
  sprintf(text, "Pontos: %d", game.score); label(panel, 48, text);
  sprintf(text, "Linhas: %d", game.lines); label(panel, 72, text);
  sprintf(text, "Nivel: %d", game.lines / 10 + 1); label(panel, 96, text);
  label(panel, 128, "Proxima:");
  for (b = 0; b < 4; b++) for (a = 0; a < 4; a++)
    if (game.cell(game.next, 0, a, b))
      block(panel + a * 16, 152 + b * 16, 16, game.next + 1);
  label(panel, 232, "Setas: mover/girar");
  label(panel, 256, "Espaco: cair");
  label(panel, 280, "P: pausa");
  label(panel, 304, "R: reiniciar");
  label(panel, 328, "Esc: sair");
  if (game.over) label(panel, 368, "FIM DE JOGO");
  else if (game.paused) label(panel, 368, "PAUSADO");
  gui_present();
}

int main() {
  gui_event_t ev;
  unsigned last, now;
  int key, oldw, oldh, dirty;
  if (gui_open("Tetris") != 0) {
    k_puts("Abra TETRIS.EXE pelo desktop WIN.\n");
    return 1;
  }
  game.random = k_ms();
  game.reset(); last = k_ms();
  oldw = gui_width(); oldh = gui_height();
  draw();
  for (;;) {
    dirty = 0;
    gui_wait(&ev, 20);
    if (ev.type == GUI_EV_CLOSE) break;
    if (ev.type == GUI_EV_KEY) {
      key = ev.key;
      if (key == 27) break;
      if (key == 'r' || key == 'R') { game.reset(); last = k_ms(); }
      else if ((key == 'p' || key == 'P') && !game.over) {
        game.paused = !game.paused; last = k_ms();
      } else if (!game.over && !game.paused) {
        if (key == GUI_KEY_LEFT && game.fits(game.x - 1, game.y, game.rotation))
          game.x--;
        if (key == GUI_KEY_RIGHT && game.fits(game.x + 1, game.y, game.rotation))
          game.x++;
        if (key == GUI_KEY_UP) game.turn();
        if (key == GUI_KEY_DOWN) {
          if (game.down()) game.score++;
          last = k_ms();
        }
        if (key == ' ') {
          while (game.fits(game.x, game.y + 1, game.rotation)) {
            game.y++; game.score += 2;
          }
          game.lock(); last = k_ms();
        }
      }
      dirty = 1;
    }
    now = k_ms();
    if (!game.over && !game.paused && now - last >= (unsigned)game.interval()) {
      game.down(); last = now; dirty = 1;
    }
    if (oldw != gui_width() || oldh != gui_height()) {
      oldw = gui_width(); oldh = gui_height(); dirty = 1;
    }
    if (dirty) draw();
  }
  gui_close();
  return 0;
}
