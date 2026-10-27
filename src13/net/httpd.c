/* HTTP/1.0 na porta pedida. Uma resposta e a conexao fecha.
 * GET /exec?cmd=...   POST /exec com o comando no corpo
 * GET /mouse          POST /mouse  x=&y=&click=1
 * POST /click         mesmo que o mouse com clique */
#include "httpd.h"
#include "net.h"
#include "shell.h"
#include "visual.h"
#include "serial.h"
#include "mm.h"

static int listening;
static int port_num;
static int poll_lock;
static int in_cmd;

static int eq(const char* a, const char* b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static int starts(const char* s, const char* p) {
  while (*p) {
    if (*s != *p) return 0;
    s++;
    p++;
  }
  return 1;
}

static int hexd(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void urldec(char* s) {
  char* w = s;
  while (*s) {
    if (*s == '+') {
      *w++ = ' ';
      s++;
    } else if (*s == '%' && hexd(s[1]) >= 0 && hexd(s[2]) >= 0) {
      *w++ = (char)((hexd(s[1]) << 4) | hexd(s[2]));
      s += 3;
    } else {
      *w++ = *s++;
    }
  }
  *w = 0;
}

static int form_get(const char* s, const char* key, char* dst, int max) {
  int klen = 0;
  if (!s || !key || !dst || max < 2) return 0;
  while (key[klen]) klen++;
  while (*s) {
    int i = 0;
    while (i < klen && s[i] == key[i]) i++;
    if (i == klen && s[i] == '=') {
      int n = 0;
      s += i + 1;
      while (*s && *s != '&' && n < max - 1) dst[n++] = *s++;
      dst[n] = 0;
      urldec(dst);
      return 1;
    }
    while (*s && *s != '&') s++;
    if (*s == '&') s++;
  }
  return 0;
}

static int num_of(const char* s, int* out) {
  int sign = 1;
  int v = 0;
  int any = 0;
  if (!s || !s[0]) return 0;
  if (*s == '-') { sign = -1; s++; }
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
    any = 1;
  }
  if (!any) return 0;
  *out = sign < 0 ? -v : v;
  return 1;
}

static char cap_buf[4608];
static int cap_n;

static void cap_putc(char c) {
  if (cap_n < (int)sizeof(cap_buf) - 1) cap_buf[cap_n++] = c;
}

static int is_win_word(const char* s) {
  char b[8];
  int i;
  for (i = 0; i < 7 && s[i] && s[i] != ' '; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    b[i] = c;
  }
  b[i] = 0;
  return eq(b, "win") || eq(b, "visual");
}

/* 1 = a conexao ainda e a mesma e a resposta pode sair. */
static int run_cmd(const char* cmd, char* out, int* outn, int max) {
  char line[240];
  static char saved[4608];
  int saved_n = 0;
  int i = 0;
  int gen;
  int nested;
  void (*prev)(char c);
  while (cmd[i] && cmd[i] != '\r' && cmd[i] != '\n' && i < (int)sizeof(line) - 1) {
    line[i] = cmd[i];
    i++;
  }
  line[i] = 0;
  if (in_cmd && is_win_word(line)) {
    const char* m = "win ja aberto\n";
    int n = 0;
    while (m[n] && n < max - 1) { out[n] = m[n]; n++; }
    out[n] = 0;
    *outn = n;
    return 1;
  }
  nested = in_cmd;
  if (nested) {
    saved_n = cap_n;
    if (saved_n > (int)sizeof(saved)) saved_n = (int)sizeof(saved);
    for (i = 0; i < saved_n; i++) saved[i] = cap_buf[i];
  }
  cap_n = 0;
  prev = shell_get_out();
  shell_set_out(cap_putc);
  gen = (int)net_srv_gen();
  in_cmd = 1;
  poll_lock = 0;
  shell_exec_line(line);
  poll_lock = 1;
  in_cmd = nested;
  shell_set_out(prev);
  if ((int)net_srv_gen() != gen) {
    if (nested) {
      cap_n = saved_n;
      for (i = 0; i < saved_n; i++) cap_buf[i] = saved[i];
    }
    return 0;
  }
  if (cap_n > max - 1) cap_n = max - 1;
  for (i = 0; i < cap_n; i++) out[i] = cap_buf[i];
  out[cap_n] = 0;
  *outn = cap_n;
  if (nested) {
    cap_n = saved_n;
    for (i = 0; i < saved_n; i++) cap_buf[i] = saved[i];
  }
  return 1;
}

static void add_str(char* d, int* n, int max, const char* s) {
  while (*s && *n < max - 1) d[(*n)++] = *s++;
  d[*n] = 0;
}

static void add_num(char* d, int* n, int max, int v) {
  char tmp[16];
  int32_to_string(v, tmp, (int)sizeof(tmp));
  add_str(d, n, max, tmp);
}

static void reply(int code, const char* reason, const char* body, int blen, int head) {
  char pkt[9000];
  int n = 0;
  char nb[16];
  int sendn;
  if (blen < 0) blen = 0;
  sendn = head ? 0 : blen;
  add_str(pkt, &n, (int)sizeof(pkt), "HTTP/1.0 ");
  add_num(pkt, &n, (int)sizeof(pkt), code);
  add_str(pkt, &n, (int)sizeof(pkt), " ");
  add_str(pkt, &n, (int)sizeof(pkt), reason);
  add_str(pkt, &n, (int)sizeof(pkt), "\r\nContent-Type: text/plain\r\nContent-Length: ");
  int32_to_string(blen, nb, (int)sizeof(nb));
  add_str(pkt, &n, (int)sizeof(pkt), nb);
  add_str(pkt, &n, (int)sizeof(pkt), "\r\nConnection: close\r\n\r\n");
  if (body) {
    int i;
    for (i = 0; i < sendn && n < (int)sizeof(pkt) - 1; i++) pkt[n++] = body[i];
  }
  net_srv_send(pkt, n);
}

static const char PAGE[] =
  "SO DOS HTTP\n"
  "GET /win\n"
  "GET /win/printscreen\n"
  "GET /win/mouse/coord\n"
  "GET /win/mouse?x=10&y=20&click=1\n"
  "GET /win/windows\n"
  "GET /win/close?program=Gerenciador\n"
  "GET /win/execute?cmd=help\n"
  "POST /win/execute   corpo = comando\n"
  "GET /exec?cmd=help\n"
  "POST /exec   corpo = comando\n"
  "POST /mouse  x=10&y=20&click=1\n";

static void put_u16(uint8_t* p, unsigned v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t* p, unsigned v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static int send_hdr(int code, const char* reason, const char* ctype, int blen, int head) {
  char pkt[256];
  int n = 0;
  char nb[16];
  add_str(pkt, &n, (int)sizeof(pkt), "HTTP/1.0 ");
  add_num(pkt, &n, (int)sizeof(pkt), code);
  add_str(pkt, &n, (int)sizeof(pkt), " ");
  add_str(pkt, &n, (int)sizeof(pkt), reason);
  add_str(pkt, &n, (int)sizeof(pkt), "\r\nContent-Type: ");
  add_str(pkt, &n, (int)sizeof(pkt), ctype);
  add_str(pkt, &n, (int)sizeof(pkt), "\r\nContent-Length: ");
  int32_to_string(blen, nb, (int)sizeof(nb));
  add_str(pkt, &n, (int)sizeof(pkt), nb);
  add_str(pkt, &n, (int)sizeof(pkt), "\r\nConnection: close\r\n\r\n");
  return net_srv_write(pkt, n, head || blen == 0);
}

static void send_bmp(void) {
  int w = 0, h = 0, y, row, x;
  unsigned imagesz, filesz;
  uint8_t hdr[54];
  uint8_t* line;
  if (!win_screen_size(&w, &h) || w < 1 || h < 1 || w > 2048) {
    reply(404, "Not Found", "win fechado\n", 12, 0);
    return;
  }
  row = (w * 3 + 3) & ~3;
  /* Fora do BSS: o nucleo tem de acabar antes do slot 0x58000. */
  line = (uint8_t*)kmalloc((uint32_t)row);
  if (!line) {
    reply(500, "Error", "sem memoria\n", 12, 0);
    return;
  }
  imagesz = (unsigned)row * (unsigned)h;
  filesz = 54u + imagesz;
  for (x = 0; x < 54; x++) hdr[x] = 0;
  hdr[0] = 'B';
  hdr[1] = 'M';
  put_u32(hdr + 2, filesz);
  put_u32(hdr + 10, 54);
  put_u32(hdr + 14, 40);
  put_u32(hdr + 18, (unsigned)w);
  put_u32(hdr + 22, (unsigned)h);
  put_u16(hdr + 26, 1);
  put_u16(hdr + 28, 24);
  put_u32(hdr + 34, imagesz);
  if (send_hdr(200, "OK", "image/bmp", (int)filesz, 0) < 0 ||
      net_srv_write(hdr, 54, 0) < 0) {
    kfree(line);
    return;
  }
  for (y = h - 1; y >= 0; y--) {
    int n = win_screen_bgr(y, line, w);
    if (n < 0) n = 0;
    for (x = n * 3; x < row; x++) line[x] = 0;
    if (net_srv_write(line, row, y == 0) < 0) break;
  }
  kfree(line);
}

static void handle(char* req) {
  char method[8];
  char path[80];
  char* sp;
  char* q;
  char* body;
  int i, mi, head;
  char out[6000];
  int outn = 0;
  for (i = 0; i < 7 && req[i] && req[i] != ' '; i++) {
    char c = req[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    method[i] = c;
  }
  method[i] = 0;
  mi = i;
  if (req[mi] != ' ') {
    reply(400, "Bad Request", "pedido\n", 7, 0);
    return;
  }
  sp = req + mi + 1;
  i = 0;
  while (sp[i] && sp[i] != ' ' && i < (int)sizeof(path) - 1) {
    path[i] = sp[i];
    i++;
  }
  path[i] = 0;
  q = 0;
  for (i = 0; path[i]; i++) {
    if (path[i] == '?') {
      path[i] = 0;
      q = path + i + 1;
      break;
    }
  }
  body = 0;
  for (i = 0; req[i]; i++) {
    if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n') {
      body = req + i + 4;
      break;
    }
  }
  head = eq(method, "HEAD");
  if (!eq(method, "GET") && !eq(method, "POST") && !eq(method, "PUT") &&
      !eq(method, "DELETE") && !head) {
    reply(405, "Method Not Allowed", "metodo\n", 7, 0);
    return;
  }
  serial_puts("[httpd] ");
  serial_puts(method);
  serial_puts(" ");
  serial_puts(path);
  serial_puts("\n");
  if (eq(path, "/") || eq(path, "")) {
    reply(200, "OK", PAGE, (int)(sizeof(PAGE) - 1), head);
    return;
  }
  if (eq(path, "/exec")) {
    char cmd[240];
    cmd[0] = 0;
    if (!form_get(q, "cmd", cmd, (int)sizeof(cmd)))
      form_get(body, "cmd", cmd, (int)sizeof(cmd));
    if (!cmd[0] && body && body[0]) {
      int n = 0;
      while (body[n] && body[n] != '\r' && body[n] != '\n' && n < (int)sizeof(cmd) - 1) {
        cmd[n] = body[n];
        n++;
      }
      cmd[n] = 0;
      urldec(cmd);
    }
    if (!cmd[0]) {
      reply(400, "Bad Request", "comando vazio\n", 14, head);
      return;
    }
    if (run_cmd(cmd, out, &outn, (int)sizeof(out)))
      reply(200, "OK", out, outn, head);
    return;
  }
  if (eq(path, "/win")) {
    const char* t =
      "GET /win/printscreen\n"
      "GET /win/mouse/coord\n"
      "GET /win/mouse?x=10&y=20&click=1\n"
      "GET /win/windows\n"
      "GET /win/close?program=Gerenciador\n"
      "GET /win/execute?cmd=dir\n"
      "POST /win/execute\n"
      "POST /win/close\n";
    int n = 0;
    while (t[n]) n++;
    reply(200, "OK", t, n, head);
    return;
  }
  if (eq(path, "/win/printscreen")) {
    if (head) reply(200, "OK", 0, 0, 1);
    else send_bmp();
    return;
  }
  if (eq(path, "/win/mouse/coord") || eq(path, "/win/windows") ||
      eq(path, "/win/close") || eq(path, "/win/execute") ||
      eq(path, "/win/mouse")) {
    if (eq(path, "/win/mouse/coord")) {
      int x = 0, y = 0;
      mouse_api_pos(&x, &y);
      outn = 0;
      add_str(out, &outn, (int)sizeof(out), "x ");
      add_num(out, &outn, (int)sizeof(out), x);
      add_str(out, &outn, (int)sizeof(out), " y ");
      add_num(out, &outn, (int)sizeof(out), y);
      add_str(out, &outn, (int)sizeof(out), "\n");
      reply(200, "OK", out, outn, head);
      return;
    }
    if (eq(path, "/win/windows")) {
      outn = win_windows(out, (int)sizeof(out));
      if (outn < 0) reply(404, "Not Found", "win fechado\n", 12, head);
      else {
        int n = 0;
        while (out[n]) n++;
        reply(200, "OK", out, n, head);
      }
      return;
    }
    if (eq(path, "/win/close")) {
      char name[32];
      name[0] = 0;
      if (!form_get(q, "program", name, (int)sizeof(name)))
        if (!form_get(body, "program", name, (int)sizeof(name)))
          if (!form_get(q, "title", name, (int)sizeof(name)))
            form_get(body, "title", name, (int)sizeof(name));
      if (!name[0]) {
        reply(400, "Bad Request", "program vazio\n", 14, head);
        return;
      }
      if (win_close(name) == 0) reply(200, "OK", "fechou\n", 7, head);
      else reply(404, "Not Found", "nao achei\n", 10, head);
      return;
    }
    if (eq(path, "/win/execute")) {
      char cmd[240];
      cmd[0] = 0;
      if (!form_get(q, "cmd", cmd, (int)sizeof(cmd)))
        form_get(body, "cmd", cmd, (int)sizeof(cmd));
      if (!cmd[0] && body && body[0]) {
        int n = 0;
        while (body[n] && body[n] != '\r' && body[n] != '\n' && n < (int)sizeof(cmd) - 1) {
          cmd[n] = body[n];
          n++;
        }
        cmd[n] = 0;
        urldec(cmd);
      }
      if (!cmd[0]) {
        reply(400, "Bad Request", "comando vazio\n", 14, head);
        return;
      }
      if (run_cmd(cmd, out, &outn, (int)sizeof(out)))
        reply(200, "OK", out, outn, head);
      return;
    }
  }
  if (eq(path, "/mouse") || eq(path, "/click") || eq(path, "/win/mouse")) {
    char xs[16], ys[16], cs[16];
    int x = 0, y = 0, click = 0, have = 0;
    mouse_api_pos(&x, &y);
    xs[0] = ys[0] = cs[0] = 0;
    if (form_get(q, "x", xs, (int)sizeof(xs)) || form_get(body, "x", xs, (int)sizeof(xs))) {
      if (num_of(xs, &x)) have = 1;
    }
    if (form_get(q, "y", ys, (int)sizeof(ys)) || form_get(body, "y", ys, (int)sizeof(ys))) {
      if (num_of(ys, &y)) have = 1;
    }
    if (eq(path, "/click")) click = 1;
    if (form_get(q, "click", cs, (int)sizeof(cs)) || form_get(body, "click", cs, (int)sizeof(cs)))
      num_of(cs, &click);
    if (have || click || eq(method, "POST") || eq(method, "PUT"))
      mouse_api(x, y, click ? 1 : 0);
    mouse_api_pos(&x, &y);
    outn = 0;
    add_str(out, &outn, (int)sizeof(out), "x ");
    add_num(out, &outn, (int)sizeof(out), x);
    add_str(out, &outn, (int)sizeof(out), " y ");
    add_num(out, &outn, (int)sizeof(out), y);
    add_str(out, &outn, (int)sizeof(out), click ? " click 1\n" : " click 0\n");
    reply(200, "OK", out, outn, head);
    return;
  }
  reply(404, "Not Found", "nao achei\n", 10, head);
}

void httpd_start(int port) {
  if (!net_ready()) {
    puts_k("HTTP: sem rede\n");
    return;
  }
  port_num = port;
  net_srv_listen(port);
  listening = 1;
  puts_k("HTTP na porta 8797. GET /   POST /exec   POST /mouse\n");
  serial_puts("[httpd] escuta 8797\n");
}

void httpd_poll(void) {
  static char req[8192];
  int n;
  if (!listening || poll_lock) return;
  poll_lock = 1;
  n = net_srv_take(req, (int)sizeof(req));
  if (n > 0) handle(req);
  poll_lock = 0;
  (void)port_num;
}
