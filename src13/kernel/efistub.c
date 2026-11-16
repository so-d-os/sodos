/* efistub.c - diagnostico de video com os boot services do UEFI ainda vivos.
 *
 * O cabecalho Multiboot2 (kernel/multiboot_header.asm) pede as tags 7 e 9:
 * num GRUB UEFI de 64 bits o GRUB nao chama ExitBootServices e salta para
 * efi_entry em long mode, com a paginacao (identidade) do firmware. Ali
 * efistub_main(mbi) roda ANTES do kernel:
 *
 *   1. lista as placas de video PCI (classe 03) e manda o firmware ligar os
 *      drivers delas (ConnectController recursivo, como o "connect -r" do
 *      shell EFI): sem isto a GOP da placa que nao e a primaria nem carrega;
 *   2. lista cada saida GOP: caminho do dispositivo, modo atual, todos os
 *      modos que ela aceita e o EDID do monitor (descoberto e ativo);
 *   3. chama ExitBootServices e volta para o asm, que sai do long mode e
 *      segue pelo caminho de sempre (multiboot_entry em 32 bits).
 *
 * Nada aqui troca o modo de video: so le. O texto fica em efi_log (no .data:
 * o kmain zera o .bss) e o bootinfo_set() o manda para a serial, ou seja,
 * para o \LOG\SERIAL.LOG do PC real.
 *
 * Roda antes do kernel: nao chama nada do kernel (nem serial_puts, cujo
 * buffer fica no .bss). No BIOS, no "make run" (stage2) e em GRUB sem UEFI
 * nada disto executa e efi_log fica vazio.
 */
#include <stdint.h>
#include "efistub.h"

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void* EFI_HANDLE;

typedef struct {
  uint32_t d1;
  uint16_t d2, d3;
  uint8_t d4[8];
} EFI_GUID;

#define EFI_ERR(st) ((int64_t)(st) < 0)

/* Tabela do sistema (so o que usamos; offsets do x64). */
typedef struct {
  uint8_t hdr[24];
  void* firmware_vendor;           /* 24 */
  uint32_t firmware_revision;      /* 32 */
  uint32_t pad;
  EFI_HANDLE con_in_handle;        /* 40 */
  void* con_in;                    /* 48 */
  EFI_HANDLE con_out_handle;       /* 56 */
  void* con_out;                   /* 64 */
  EFI_HANDLE std_err_handle;       /* 72 */
  void* std_err;                   /* 80 */
  void* runtime;                   /* 88 */
  struct EFI_BOOT_SERVICES* boot;  /* 96 */
} EFI_SYSTEM_TABLE;

typedef struct EFI_BOOT_SERVICES {
  uint8_t hdr[24];
  void* raise_tpl;
  void* restore_tpl;
  void* allocate_pages;
  void* free_pages;
  EFI_STATUS (EFIAPI* get_memory_map)(UINTN* size, void* map, UINTN* key,
                                      UINTN* desc_size, uint32_t* desc_ver);
  void* allocate_pool;
  EFI_STATUS (EFIAPI* free_pool)(void* p);
  void* create_event;
  void* set_timer;
  void* wait_for_event;
  void* signal_event;
  void* close_event;
  void* check_event;
  void* install_protocol_interface;
  void* reinstall_protocol_interface;
  void* uninstall_protocol_interface;
  EFI_STATUS (EFIAPI* handle_protocol)(EFI_HANDLE h, const EFI_GUID* g, void** iface);
  void* reserved;
  void* register_protocol_notify;
  void* locate_handle;
  void* locate_device_path;
  void* install_configuration_table;
  void* load_image;
  void* start_image;
  void* exit;
  void* unload_image;
  EFI_STATUS (EFIAPI* exit_boot_services)(EFI_HANDLE image, UINTN key);
  void* get_next_monotonic_count;
  void* stall;
  void* set_watchdog_timer;
  EFI_STATUS (EFIAPI* connect_controller)(EFI_HANDLE h, EFI_HANDLE* drivers,
                                          void* remaining, uint8_t recursive);
  void* disconnect_controller;
  void* open_protocol;
  void* close_protocol;
  void* open_protocol_information;
  void* protocols_per_handle;
  EFI_STATUS (EFIAPI* locate_handle_buffer)(int search, const EFI_GUID* g, void* key,
                                            UINTN* n, EFI_HANDLE** buf);
} EFI_BOOT_SERVICES;

#define BY_PROTOCOL 2

typedef struct {
  uint32_t version;
  uint32_t w, h;
  uint32_t pixel_format;   /* 0 RGB, 1 BGR, 2 bitmask, 3 so Blt (sem LFB) */
  uint32_t mask[4];
  uint32_t pixels_per_line;
} GOP_INFO;

typedef struct {
  uint32_t max_mode;
  uint32_t mode;
  GOP_INFO* info;
  UINTN size_of_info;
  uint64_t fb_base;
  UINTN fb_size;
} GOP_MODE;

typedef struct GOP {
  EFI_STATUS (EFIAPI* query_mode)(struct GOP* g, uint32_t mode, UINTN* size, GOP_INFO** info);
  EFI_STATUS (EFIAPI* set_mode)(struct GOP* g, uint32_t mode);
  void* blt;
  GOP_MODE* mode;
} GOP;

typedef struct PCI_IO {
  void* poll_mem;
  void* poll_io;
  void* mem_read;
  void* mem_write;
  void* io_read;
  void* io_write;
  EFI_STATUS (EFIAPI* pci_read)(struct PCI_IO* p, int width, uint32_t off, UINTN n, void* buf);
  void* pci_write;
  void* copy_mem;
  void* map;
  void* unmap;
  void* allocate_buffer;
  void* free_buffer;
  void* flush;
  EFI_STATUS (EFIAPI* get_location)(struct PCI_IO* p, UINTN* seg, UINTN* bus, UINTN* dev, UINTN* fn);
  void* attributes;
  void* get_bar_attributes;
  void* set_bar_attributes;
  uint64_t rom_size;
  void* rom_image;
} PCI_IO;

typedef struct {
  uint32_t size;
  uint8_t* edid;
} EDID_PROTO;

static const EFI_GUID guid_gop =
  { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static const EFI_GUID guid_pci_io =
  { 0x4cf5b200, 0x68b8, 0x4ca5, { 0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a } };
static const EFI_GUID guid_dpath =
  { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const EFI_GUID guid_edid_disc =
  { 0x1c0c34f6, 0xd380, 0x41fa, { 0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa } };
static const EFI_GUID guid_edid_act =
  { 0xbd8c1056, 0x9f36, 0x44ec, { 0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } };

/* ---------- texto (no .data: sobrevive ao memset do .bss no kmain) ---------- */

#define LOG_MAX 12288
static char efi_log[LOG_MAX] __attribute__((section(".data")));
static uint32_t efi_log_n __attribute__((section(".data")));

/* Mapa de memoria so para pegar a chave do ExitBootServices: rascunho no
 * .bss (o kmain zera depois, nao importa). Fixo para nao alocar pool e mudar
 * o proprio mapa entre GetMemoryMap e ExitBootServices. */
static uint8_t mmap_buf[64 * 1024] __attribute__((aligned(16)));

static void lc(char c) {
  if (efi_log_n < LOG_MAX - 1) efi_log[efi_log_n++] = c;
}

static void ls(const char* s) {
  while (*s) lc(*s++);
}

static void lhex(uint64_t v, int digits) {
  int i;
  for (i = digits - 1; i >= 0; i--) lc("0123456789ABCDEF"[(v >> (i * 4)) & 15u]);
}

static void ldec(uint64_t v) {
  char t[21];
  int n = 0;
  do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 20);
  while (n) lc(t[--n]);
}

static void lst(EFI_STATUS st) {
  if (!st) { ls("ok"); return; }
  ls("erro ");
  lhex(st & 0xFFu, 2);
}

/* ---------- MBI ---------- */

static const uint8_t* mbi_tag(const uint8_t* mbi, uint32_t type) {
  uint32_t total = *(const uint32_t*)mbi;
  uint32_t off = 8;
  while (off + 8 <= total) {
    uint32_t t = *(const uint32_t*)(mbi + off);
    uint32_t sz = *(const uint32_t*)(mbi + off + 4);
    if (t == 0 || sz < 8) break;
    if (t == type) return mbi + off;
    off = (off + sz + 7u) & ~7u;
  }
  return 0;
}

/* ---------- partes do relatorio ---------- */

static EFI_BOOT_SERVICES* bs;

/* "PciRoot(0)/Pci(1,1)/Pci(0,0)/AcpiAdr(80010100)" */
static void log_dpath(EFI_HANDLE h) {
  const uint8_t* p;
  int n = 0;
  if (EFI_ERR(bs->handle_protocol(h, &guid_dpath, (void**)&p)) || !p) {
    ls("(sem caminho de dispositivo)");
    return;
  }
  while (n++ < 16) {
    uint8_t type = p[0], sub = p[1];
    uint16_t len = (uint16_t)(p[2] | p[3] << 8);
    if (type == 0x7F || len < 4) break;
    if (n > 1) lc('/');
    if (type == 2 && sub == 1 && len >= 12) {
      uint32_t hid = *(const uint32_t*)(p + 4), uid = *(const uint32_t*)(p + 8);
      if (hid == 0x0A0341D0u || hid == 0x0A0841D0u) ls("PciRoot(");
      else { ls("Acpi("); lhex(hid, 8); lc(','); }
      ldec(uid);
      lc(')');
    } else if (type == 1 && sub == 1 && len >= 6) {
      ls("Pci(");
      lhex(p[5], 2);
      lc(',');
      ldec(p[4]);
      lc(')');
    } else if (type == 2 && sub == 3 && len >= 8) {
      ls("AcpiAdr(");
      lhex(*(const uint32_t*)(p + 4), 8);
      lc(')');
    } else {
      ls("T");
      lhex(type, 2);
      lc('.');
      lhex(sub, 2);
    }
    p += len;
  }
}

/* "EDID ativo 256 B: GSM5C85, nativo 1920x1080 148.50 MHz" */
static void log_edid(EFI_HANDLE h, const EFI_GUID* g, const char* what) {
  EDID_PROTO* e;
  const uint8_t* d;
  uint32_t clk;
  ls("    EDID ");
  ls(what);
  ls(": ");
  if (EFI_ERR(bs->handle_protocol(h, g, (void**)&e)) || !e) {
    ls("nao ha\n");
    return;
  }
  ldec(e->size);
  ls(" B");
  d = e->edid;
  if (e->size < 128 || !d) {
    lc('\n');
    return;
  }
  {
    uint16_t m = (uint16_t)(d[8] << 8 | d[9]);
    ls(", ");
    lc((char)('@' + ((m >> 10) & 31)));
    lc((char)('@' + ((m >> 5) & 31)));
    lc((char)('@' + (m & 31)));
    lhex((uint32_t)(d[10] | d[11] << 8), 4);
  }
  clk = (uint32_t)(d[54] | d[55] << 8);
  if (clk) {
    ls(", nativo ");
    ldec((uint32_t)(d[56] | (d[58] & 0xF0u) << 4));
    lc('x');
    ldec((uint32_t)(d[59] | (d[61] & 0xF0u) << 4));
    lc(' ');
    ldec(clk / 100u);
    lc('.');
    lc((char)('0' + clk / 10u % 10u));
    lc((char)('0' + clk % 10u));
    ls(" MHz");
  }
  lc('\n');
}

/* Placas de video PCI; ligar=1 manda o firmware conectar os drivers. */
static int pci_displays(int ligar) {
  EFI_HANDLE* hs = 0;
  UINTN n = 0, i;
  int found = 0;
  EFI_STATUS st = bs->locate_handle_buffer(BY_PROTOCOL, &guid_pci_io, 0, &n, &hs);
  if (EFI_ERR(st) || !hs) {
    ls("[efi] PCI IO: ");
    lst(st);
    lc('\n');
    return 0;
  }
  for (i = 0; i < n; i++) {
    PCI_IO* pio;
    uint32_t id = 0, cls = 0;
    UINTN seg = 0, bus = 0, dev = 0, fn = 0;
    if (EFI_ERR(bs->handle_protocol(hs[i], &guid_pci_io, (void**)&pio)) || !pio) continue;
    if (EFI_ERR(pio->pci_read(pio, 2, 0, 1, &id))) continue;
    if (EFI_ERR(pio->pci_read(pio, 2, 8, 1, &cls))) continue;
    if ((cls >> 24) != 0x03) continue;
    found++;
    pio->get_location(pio, &seg, &bus, &dev, &fn);
    ls("[efi] video PCI ");
    lhex(bus, 2);
    lc(':');
    lhex(dev, 2);
    lc('.');
    ldec(fn);
    ls(" id ");
    lhex(id & 0xFFFFu, 4);
    lc(':');
    lhex(id >> 16, 4);
    ls(" classe ");
    lhex(cls >> 8, 6);
    ls(" ROM ");
    ldec(pio->rom_size / 1024u);
    ls(" KB");
    if (ligar) {
      st = bs->connect_controller(hs[i], 0, 0, 1);
      ls(", connect ");
      if ((st & 0xFFu) == 14u && EFI_ERR(st)) ls("nada novo (drivers ja ligados)");
      else lst(st);
    }
    lc('\n');
  }
  bs->free_pool(hs);
  return found;
}

static UINTN gop_count(void) {
  EFI_HANDLE* hs = 0;
  UINTN n = 0;
  if (EFI_ERR(bs->locate_handle_buffer(BY_PROTOCOL, &guid_gop, 0, &n, &hs)) || !hs) return 0;
  bs->free_pool(hs);
  return n;
}

static void gop_list(EFI_HANDLE con_out, uint64_t grub_fb) {
  EFI_HANDLE* hs = 0;
  UINTN n = 0, i;
  EFI_STATUS st = bs->locate_handle_buffer(BY_PROTOCOL, &guid_gop, 0, &n, &hs);
  if (EFI_ERR(st) || !hs) {
    ls("[efi] nenhuma GOP: ");
    lst(st);
    lc('\n');
    return;
  }
  for (i = 0; i < n; i++) {
    GOP* g;
    GOP_MODE* m;
    uint32_t k, per_line = 0;
    ls("[efi] GOP ");
    ldec(i);
    ls(": ");
    log_dpath(hs[i]);
    if (hs[i] == con_out) ls(" [ConOut]");
    lc('\n');
    if (EFI_ERR(bs->handle_protocol(hs[i], &guid_gop, (void**)&g)) || !g || !g->mode) {
      ls("    (sem protocolo GOP)\n");
      continue;
    }
    m = g->mode;
    ls("    modo ");
    ldec(m->mode);
    ls(" de ");
    ldec(m->max_mode);
    if (m->info) {
      ls(": ");
      ldec(m->info->w);
      lc('x');
      ldec(m->info->h);
      ls(" fmt ");
      ldec(m->info->pixel_format);
      ls(" linha ");
      ldec(m->info->pixels_per_line);
    }
    ls(", fb ");
    lhex(m->fb_base, 10);
    ls(" (");
    ldec(m->fb_size / 1024u);
    ls(" KB)");
    if (grub_fb && m->fb_base == grub_fb) ls(" <- a do GRUB");
    lc('\n');
    log_edid(hs[i], &guid_edid_disc, "descoberto");
    log_edid(hs[i], &guid_edid_act, "ativo");
    ls("    modos:");
    for (k = 0; k < m->max_mode && k < 80; k++) {
      GOP_INFO* inf = 0;
      UINTN sz = 0;
      if (per_line == 0) ls("\n     ");
      lc(' ');
      if (EFI_ERR(g->query_mode(g, k, &sz, &inf)) || !inf) {
        ls("?");
      } else {
        ldec(inf->w);
        lc('x');
        ldec(inf->h);
        if (inf->pixel_format == 3) lc('b');
        bs->free_pool(inf);
      }
      if (k == m->mode) lc('*');
      if (++per_line == 8) per_line = 0;
    }
    lc('\n');
  }
  bs->free_pool(hs);
}

/* A tela que o kernel recebe. Com a tag 7 o GRUB nao garante que a tag de
 * framebuffer bata com o modo da placa (no QEMU ele programa a bochs direto,
 * a 24 bpp, sem a GOP saber). Pega a GOP do endereco da tag (senao a
 * primeira com caminho de dispositivo e framebuffer linear), poe nela o
 * tamanho que o GRUB escolheu a 32 bpp se a placa tiver esse modo, e
 * reescreve a tag com o que a GOP diz. Se ja bate, nao mexe em nada. */
static void fb_fix(uint8_t* t_fb) {
  EFI_HANDLE* hs = 0;
  UINTN n = 0, i;
  GOP* pick = 0;
  GOP_MODE* m;
  uint32_t want_w, want_h, k, set = 0xFFFFFFFFu;
  uint64_t tag_addr;
  if (!t_fb || *(const uint32_t*)(t_fb + 4) < 38) {
    ls("[efi] sem tag de framebuffer completa: a tela fica como esta\n");
    return;
  }
  tag_addr = *(const uint64_t*)(t_fb + 8);
  want_w = *(const uint32_t*)(t_fb + 20);
  want_h = *(const uint32_t*)(t_fb + 24);
  if (EFI_ERR(bs->locate_handle_buffer(BY_PROTOCOL, &guid_gop, 0, &n, &hs)) || !hs) return;
  for (i = 0; i < n && !pick; i++) {
    GOP* g;
    if (EFI_ERR(bs->handle_protocol(hs[i], &guid_gop, (void**)&g)) || !g || !g->mode) continue;
    if (g->mode->fb_base == tag_addr && g->mode->info && g->mode->info->pixel_format < 2) pick = g;
  }
  for (i = 0; i < n && !pick; i++) {
    GOP* g;
    void* dp;
    if (EFI_ERR(bs->handle_protocol(hs[i], &guid_gop, (void**)&g)) || !g || !g->mode) continue;
    if (EFI_ERR(bs->handle_protocol(hs[i], &guid_dpath, &dp))) continue;
    if (g->mode->fb_base && g->mode->info && g->mode->info->pixel_format < 2) pick = g;
  }
  bs->free_pool(hs);
  if (!pick) {
    ls("[efi] nenhuma GOP com framebuffer linear: tela como o GRUB deixou\n");
    return;
  }
  m = pick->mode;
  if (m->info->w == want_w && m->info->h == want_h && m->fb_base == tag_addr && t_fb[28] == 32 &&
      *(const uint32_t*)(t_fb + 16) == m->info->pixels_per_line * 4u) {
    ls("[efi] tag do GRUB confere com a GOP\n");
    return;
  }
  for (k = 0; k < m->max_mode; k++) {
    GOP_INFO* inf = 0;
    UINTN sz = 0;
    if (EFI_ERR(pick->query_mode(pick, k, &sz, &inf)) || !inf) continue;
    if (inf->w == want_w && inf->h == want_h && inf->pixel_format < 2) set = k;
    bs->free_pool(inf);
    if (set != 0xFFFFFFFFu) break;
  }
  if (set != 0xFFFFFFFFu) {
    EFI_STATUS s = pick->set_mode(pick, set);
    ls("[efi] SetMode ");
    ldec(set);
    ls(" (");
    ldec(want_w);
    lc('x');
    ldec(want_h);
    ls("): ");
    lst(s);
    lc('\n');
  }
  m = pick->mode;
  if (!m->info || m->info->pixel_format >= 2 || !m->fb_base) return;
  *(uint64_t*)(t_fb + 8) = m->fb_base;
  *(uint32_t*)(t_fb + 16) = m->info->pixels_per_line * 4u;
  *(uint32_t*)(t_fb + 20) = m->info->w;
  *(uint32_t*)(t_fb + 24) = m->info->h;
  t_fb[28] = 32;
  t_fb[29] = 1;
  /* fmt 0: bytes R,G,B na memoria (vermelho no bit 0); fmt 1: B,G,R. */
  t_fb[32] = m->info->pixel_format == 0 ? 0 : 16;
  t_fb[33] = 8;
  t_fb[34] = 8;
  t_fb[35] = 8;
  t_fb[36] = m->info->pixel_format == 0 ? 16 : 0;
  t_fb[37] = 8;
  ls("[efi] tag de framebuffer reescrita: ");
  lhex(m->fb_base, 10);
  lc(' ');
  ldec(m->info->w);
  lc('x');
  ldec(m->info->h);
  ls(" linha ");
  ldec(m->info->pixels_per_line * 4u);
  ls(" fmt ");
  ldec(m->info->pixel_format);
  lc('\n');
}

/* ---------- entrada ---------- */

int efistub_main(const uint8_t* mbi) {
  const uint8_t* t_st = mbi_tag(mbi, 12);   /* EFI64 system table */
  const uint8_t* t_ih = mbi_tag(mbi, 20);   /* EFI64 image handle */
  const uint8_t* t_fb = mbi_tag(mbi, 8);
  EFI_SYSTEM_TABLE* st;
  EFI_HANDLE image;
  uint64_t grub_fb = 0;
  UINTN before, after;
  EFI_STATUS s = 1;
  int tries;

  efi_log_n = 0;
  ls("[efi] diagnostico de video (GRUB com boot services)\n");
  if (!t_st || !t_ih) {
    ls("[efi] MBI sem system table/image handle: nada feito\n");
    return -1;
  }
  st = (EFI_SYSTEM_TABLE*)(uintptr_t)*(const uint64_t*)(t_st + 8);
  image = (EFI_HANDLE)(uintptr_t)*(const uint64_t*)(t_ih + 8);
  bs = st->boot;
  if (t_fb) {
    grub_fb = *(const uint64_t*)(t_fb + 8);
    ls("[efi] tag de framebuffer do GRUB: ");
    lhex(grub_fb, 10);
    lc(' ');
    ldec(*(const uint32_t*)(t_fb + 20));
    lc('x');
    ldec(*(const uint32_t*)(t_fb + 24));
    ls(" linha ");
    ldec(*(const uint32_t*)(t_fb + 16));
    ls(" bpp ");
    ldec(t_fb[28]);
    ls(" tipo ");
    ldec(t_fb[29]);
    ls(" tam ");
    ldec(*(const uint32_t*)(t_fb + 4));
    lc('\n');
  } else {
    ls("[efi] o GRUB nao mandou tag de framebuffer\n");
  }

  before = gop_count();
  if (pci_displays(1) == 0) ls("[efi] nenhuma placa de video PCI\n");
  after = gop_count();
  ls("[efi] saidas GOP: ");
  ldec(before);
  ls(" antes do connect, ");
  ldec(after);
  ls(" depois\n");
  gop_list(st->con_out_handle, grub_fb);
  fb_fix((uint8_t*)t_fb);

  /* A chave muda se o mapa mudar entre as duas chamadas: tenta de novo. */
  for (tries = 0; tries < 8; tries++) {
    UINTN size = sizeof(mmap_buf), key = 0, dsz = 0;
    uint32_t dver = 0;
    s = bs->get_memory_map(&size, mmap_buf, &key, &dsz, &dver);
    if (EFI_ERR(s)) continue;
    s = st->boot->exit_boot_services(image, key);
    if (!EFI_ERR(s)) break;
  }
  ls("[efi] ExitBootServices: ");
  lst(s);
  if (tries) {
    ls(" (tentativas ");
    ldec((uint64_t)tries + 1u);
    lc(')');
  }
  lc('\n');
  return EFI_ERR(s) ? -2 : 0;
}

/* Chamado pelo bootinfo_set(), ja com o .bss zerado. */
void efistub_report(void (*puts_fn)(const char*)) {
  char line[160];
  uint32_t i = 0, n = 0;
  if (!efi_log_n || !puts_fn) return;
  for (i = 0; i < efi_log_n; i++) {
    line[n++] = efi_log[i];
    if (efi_log[i] == '\n' || n == sizeof(line) - 1) {
      line[n] = 0;
      puts_fn(line);
      n = 0;
    }
  }
  if (n) {
    line[n] = 0;
    puts_fn(line);
  }
}
