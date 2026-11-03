/* cobra.cpp - Jogo da Cobrinha (Snake) para o SODOS.
 * Modo texto 80x25. A cobra anda sozinha: um passo a cada STEP_MS ms.
 * As teclas so mudam a direcao (nao precisa mais apertar para andar).
 * Compila com:  cc -o COBRA.EXE cobra.cpp
 *
 * Controles: setas ou WASD mudam a direcao; Q sai; P pausa; R reinicia.
 * Come a fruta '*' para crescer e ganhar pontos. Bate na parede ou em si
 * mesma = fim de jogo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

#define SCR_W 80
#define SCR_H 25

/* area do jogo (dentro de uma moldura) */
#define FIELD_X 1
#define FIELD_Y 2
#define FIELD_W 78
#define FIELD_H 21

/* intervalo entre passos automaticos, em milissegundos */
#define STEP_MS 150

/* cores: fundo << 4 | frente (paleta VGA) */
#define C_BORDER  0x0A   /* verde no preto */
#define C_TITLE   0x0F   /* branco no preto */
#define C_FIELD   0x00   /* preto no preto */
#define C_SNAKE   0x0A   /* verde claro */
#define C_HEAD    0x0E   /* amarelo */
#define C_FOOD    0x0C   /* vermelho claro */
#define C_SCORE   0x0B   /* ciano */
#define C_OVER    0x4F   /* branco no vermelho */
#define C_HELP    0x07   /* cinza */

/* teclas (mesmos codigos do kernel) */
#define K_UP    0x100
#define K_DOWN  0x101
#define K_LEFT  0x102
#define K_RIGHT 0x103
#define K_ESC   27

#define MAX_LEN (FIELD_W * FIELD_H)

/* celula da tela: caractere | cor << 8 */
static unsigned short scr[SCR_H * SCR_W];

/* ---------- desenho ---------- */

static void put(int y, int x, int ch, int color) {
  if (y < 0 || y >= SCR_H || x < 0 || x >= SCR_W) return;
  scr[y * SCR_W + x] = (unsigned short)((ch & 0xFF) | ((color & 0xFF) << 8));
}

static void text(int y, int x, const char* s, int color) {
  while (*s && x < SCR_W) put(y, x++, (unsigned char)*s++, color);
}

static void fill(int y, int x, int h, int w, int ch, int color) {
  int r, c;
  for (r = 0; r < h; r++)
    for (c = 0; c < w; c++) put(y + r, x + c, ch, color);
}

static void box(int y, int x, int h, int w, int color) {
  int i;
  put(y, x, 0xDA, color);                 /* + */
  put(y, x + w - 1, 0xBF, color);
  put(y + h - 1, x, 0xC0, color);
  put(y + h - 1, x + w - 1, 0xD9, color);
  for (i = 1; i < w - 1; i++) {
    put(y, x + i, 0xC4, color);
    put(y + h - 1, x + i, 0xC4, color);
  }
  for (i = 1; i < h - 1; i++) {
    put(y + i, x, 0xB3, color);
    put(y + i, x + w - 1, 0xB3, color);
  }
}

static void flush(void) {
  k_vput(0, 0, scr, SCR_H * SCR_W);
}

/* ---------- estado do jogo ---------- */

struct pt { int x, y; };

static struct pt snake[MAX_LEN];
static int slen;
static int dir;          /* 0=cima 1=baixo 2=esq 3=direita */
static struct pt food;
static int score;
static int best;
static int over;
static int paused;

static unsigned rng_state = 12345;

static unsigned rnd(void) {
  rng_state = rng_state * 1103515245u + 12345u;
  return (rng_state >> 16) & 0x7FFF;
}

static void place_food(void) {
  int tries, i, ok;
  for (tries = 0; tries < 1000; tries++) {
    food.x = FIELD_X + (int)(rnd() % FIELD_W);
    food.y = FIELD_Y + (int)(rnd() % FIELD_H);
    ok = 1;
    for (i = 0; i < slen; i++)
      if (snake[i].x == food.x && snake[i].y == food.y) { ok = 0; break; }
    if (ok) return;
  }
  /* sem espaco livre: poe em qualquer lugar */
  food.x = FIELD_X;
  food.y = FIELD_Y;
}

static void reset_game(void) {
  int i;
  slen = 4;
  dir = 3;                       /* comeca indo para a direita */
  score = 0;
  over = 0;
  paused = 0;
  for (i = 0; i < slen; i++) {
    snake[i].x = FIELD_X + 10 - i;
    snake[i].y = FIELD_Y + FIELD_H / 2;
  }
  place_food();
}

static void draw(void) {
  char buf[64];
  int i;

  fill(0, 0, SCR_H, SCR_W, ' ', C_FIELD);
  box(0, 0, SCR_H, SCR_W, C_BORDER);

  text(0, 2, " COBRA ", C_TITLE);
  sprintf(buf, " Pontos: %d  Recorde: %d ", score, best);
  text(0, 12, buf, C_SCORE);
  text(0, SCR_W - 30, " Setas/WASD  P=pausa Q=sai ", C_HELP);

  /* fruta */
  put(food.y, food.x, '*', C_FOOD);

  /* cobra: corpo e cabeca */
  for (i = slen - 1; i >= 0; i--) {
    if (i == 0) put(snake[i].y, snake[i].x, '@', C_HEAD);
    else        put(snake[i].y, snake[i].x, 'o', C_SNAKE);
  }

  if (over) {
    int y = FIELD_Y + FIELD_H / 2 - 1;
    int x = FIELD_X + FIELD_W / 2 - 12;
    fill(y, x, 3, 24, ' ', C_OVER);
    text(y, x + 2, "  FIM DE JOGO!  ", C_OVER);
    sprintf(buf, " Pontos: %d ", score);
    text(y + 1, x + 2, buf, C_OVER);
    text(y + 2, x + 1, " R=jogar de novo Q=sair ", C_OVER);
  } else if (paused) {
    int y = FIELD_Y + FIELD_H / 2;
    int x = FIELD_X + FIELD_W / 2 - 5;
    fill(y, x, 1, 11, ' ', C_OVER);
    text(y, x + 1, " PAUSADO ", C_OVER);
  }

  flush();
}

/* ---------- logica ---------- */

/* devolve 1 se o jogo continua, 0 se acabou */
static int step(void) {
  struct pt head, nh;
  int i;

  if (over || paused) return 1;

  head = snake[0];
  nh = head;
  switch (dir) {
    case 0: nh.y--; break;
    case 1: nh.y++; break;
    case 2: nh.x--; break;
    case 3: nh.x++; break;
  }

  /* parede */
  if (nh.x < FIELD_X || nh.x >= FIELD_X + FIELD_W ||
      nh.y < FIELD_Y || nh.y >= FIELD_Y + FIELD_H) {
    over = 1;
    return 0;
  }

  /* comeu? */
  if (nh.x == food.x && nh.y == food.y) {
    if (slen < MAX_LEN) slen++;
    score += 10;
    if (score > best) best = score;
    /* desloca o corpo */
    for (i = slen - 1; i > 0; i--) snake[i] = snake[i - 1];
    snake[0] = nh;
    place_food();
    return 1;
  }

  /* colisao com o proprio corpo (ignora a cauda, que vai sair) */
  for (i = 0; i < slen - 1; i++) {
    if (snake[i].x == nh.x && snake[i].y == nh.y) {
      over = 1;
      return 0;
    }
  }

  /* move */
  for (i = slen - 1; i > 0; i--) snake[i] = snake[i - 1];
  snake[0] = nh;
  return 1;
}

/* ---------- entrada ---------- */

/* Le uma tecla sem bloquear; devolve -1 se nao houver nenhuma.
 * k_getc espera a tecla e travava o laco: so andava ao apertar algo. */
static int read_key(void) {
  int c = k_trygetc();
  if (c < 0) return -1;
  if (c == '\r') c = '\n';
  return c;
}

int main(void) {
  int running = 1;
  int need_draw = 1;
  unsigned last_step;

  /* tela de texto: se nao der, avisa e sai */
  if (k_vput(0, 0, scr, 0) != 0) {
    k_puts("COBRA precisa da tela de texto: rode fora do WIN.\n");
    return 1;
  }

  best = 0;
  reset_game();
  k_cursor(-1, 0);
  last_step = k_ms();

  while (running) {
    int c;

    /* 1) teclado: so muda a direcao, nao faz o passo */
    c = read_key();
    if (c >= 0) {
      if (c == 'q' || c == 'Q' || c == K_ESC) {
        running = 0;
        break;
      } else if (c == 'r' || c == 'R') {
        reset_game();
        last_step = k_ms();
        need_draw = 1;
      } else if (c == 'p' || c == 'P') {
        if (!over) paused = !paused;
        need_draw = 1;
      } else if (!over && !paused) {
        /* direcao: nao deixa inverter 180 graus */
        if (c == K_UP || c == 'w' || c == 'W') {
          if (dir != 1) dir = 0;
        } else if (c == K_DOWN || c == 's' || c == 'S') {
          if (dir != 0) dir = 1;
        } else if (c == K_LEFT || c == 'a' || c == 'A') {
          if (dir != 3) dir = 2;
        } else if (c == K_RIGHT || c == 'd' || c == 'D') {
          if (dir != 2) dir = 3;
        }
      }
    }

    /* 2) passo automatico por tempo */
    if (!over && !paused) {
      unsigned now = k_ms();
      if (now - last_step >= STEP_MS) {
        last_step = now;
        step();
        need_draw = 1;
      }
    }

    /* 3) redesenha quando algo mudou */
    if (need_draw) {
      draw();
      need_draw = 0;
    }
  }

  k_clear();
  k_cursor(0, 0);
  return 0;
}
