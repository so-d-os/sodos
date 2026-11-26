/* somdiag.c - SOMDIAG.EXE: diagnostico do alto-falante mudo no boot frio
 * (docs/AUDIO.md, "so depois de um boot no Linux"). Le e grava a RAM do EC
 * e fotografa os registradores do FCH da AMD que ficam alimentados num
 * reboot quente, para comparar um boot frio com um vindo do Linux.
 *
 *   SOMDIAG EC             256 bytes da RAM do EC
 *   SOMDIAG EC aa          le um byte (hexadecimal)
 *   SOMDIAG EC aa vv       grava um byte e mostra antes/depois
 *   SOMDIAG FCH            SMI, PM, PM2, IOMUX, MISC, GPIO remoto, GPIO, AOAC
 *   SOMDIAG PCI            configuracao (256 bytes) de todas as funcoes PCI
 *   SOMDIAG FOTO           EC + FCH + PCI (use com > arquivo)
 *   SOMDIAG REG            refaz as escritas do _REG do EC: SSD1/SSD2 (0x40)
 *                          a partir dos GPIOs 4 e 7
 *
 * So o "EC aa vv" e o "REG" gravam alguma coisa. Tudo roda em ring 0, e o cc
 * do SODOS nao tem asm embutido: as portas sao lidas por um trecho de codigo
 * de maquina com a porta e o valor gravados nele antes da chamada.
 */
#include <stdio.h>
#include <string.h>
#include <so.h>

/* mov dx, imm16 ; in al, dx ; movzx eax, al ; ret */
static unsigned char cod_in[] = { 0x66, 0xBA, 0, 0, 0xEC, 0x0F, 0xB6, 0xC0, 0xC3 };
/* mov dx, imm16 ; mov al, imm8 ; out dx, al ; ret */
static unsigned char cod_out[] = { 0x66, 0xBA, 0, 0, 0xB0, 0, 0xEE, 0xC3 };

static unsigned inb(unsigned port) {
  int (*f)(void) = (int (*)(void))cod_in;
  cod_in[2] = (unsigned char)port;
  cod_in[3] = (unsigned char)(port >> 8);
  return (unsigned)f() & 0xFFu;
}

static void outb(unsigned port, unsigned v) {
  void (*f)(void) = (void (*)(void))cod_out;
  cod_out[2] = (unsigned char)port;
  cod_out[3] = (unsigned char)(port >> 8);
  cod_out[5] = (unsigned char)v;
  f();
}

/* EC pelas portas do ACPI (_CRS do H_EC: 62 dados, 66 comando/estado). */
static int ec_wait(unsigned mask, unsigned want) {
  unsigned t0 = k_ms();
  for (;;) {
    if ((inb(0x66) & mask) == want) return 0;
    if ((int)(k_ms() - t0) > 200) return -1;
  }
}

static int ec_read(unsigned addr, unsigned* v) {
  if (inb(0x66) & 1u) (void)inb(0x62);              /* resposta velha na saida */
  if (ec_wait(2, 0)) return -1;
  outb(0x66, 0x80);                                 /* RD_EC */
  if (ec_wait(2, 0)) return -1;
  outb(0x62, addr);
  if (ec_wait(1, 1)) return -1;
  *v = inb(0x62);
  return 0;
}

static int ec_write(unsigned addr, unsigned v) {
  if (inb(0x66) & 1u) (void)inb(0x62);
  if (ec_wait(2, 0)) return -1;
  outb(0x66, 0x81);                                 /* WR_EC */
  if (ec_wait(2, 0)) return -1;
  outb(0x62, addr);
  if (ec_wait(2, 0)) return -1;
  outb(0x62, v);
  return ec_wait(2, 0);
}

static int ec_dump(void) {
  unsigned i, v;
  for (i = 0; i < 256; i++) {
    if (ec_read(i, &v)) {
      printf("\nec: sem resposta no byte %02X\n", i);
      return 1;
    }
    if (i % 16 == 0) printf("ec %03X:", i);
    printf(" %02X", v);
    if (i % 16 == 15) printf("\n");
  }
  return 0;
}

/* ---------- FCH da AMD: bloco ACPI MMIO em 0xFED80000 ---------- */

#define FCH 0xFED80000u

/* Os programas tem ponteiros de 32 bits e as tabelas de paginas do kernel tem
 * entradas de 64: cada entrada e lida em duas metades. Devolve a tabela do
 * nivel seguinte, ou 0 se a entrada esta ausente, e uma pagina grande (*ps)
 * ou aponta para cima de 4 GB. */
static volatile unsigned* desce(volatile unsigned* t, unsigned i, int* ps) {
  unsigned lo = t[2 * i], hi = t[2 * i + 1];
  *ps = 0;
  if (!(lo & 1u)) return 0;
  if (lo & 0x80u) {
    *ps = 1;
    return 0;
  }
  if (hi & 0x000FFFFFu) return 0;
  return (volatile unsigned*)(lo & 0xFFFFF000u);
}

/* A pagina esta mapeada? Anda nas tabelas a partir do PML4 do kernel (0x1000,
 * de kernel_entry), sem mexer nelas. O kernel mapeia MMIO em identidade. */
static int mapeado(unsigned a) {
  volatile unsigned* t = (volatile unsigned*)0x1000u;
  int ps;
  t = desce(t, 0, &ps);
  if (!t) return 0;
  t = desce(t, a >> 30, &ps);
  if (!t) return ps;
  t = desce(t, (a >> 21) & 511u, &ps);
  if (!t) return ps;
  return (int)(t[2 * ((a >> 12) & 511u)] & 1u);
}

/* Mapeia a pagina de 2 MB de a em identidade, sem cache, como o mmio_map do
 * kernel (xhci.c), mas so quando a tabela de 2 MB (PD) ja existe e a entrada
 * esta vazia: nao aloca tabela nem troca entrada presente. Entrada que passa
 * de ausente a presente nao precisa de invlpg (a CPU nao guarda ausentes). */
static int mapeia(unsigned a) {
  volatile unsigned* t = (volatile unsigned*)0x1000u;
  unsigned i = (a >> 21) & 511u;
  int ps;
  t = desce(t, 0, &ps);
  if (!t) return -1;
  t = desce(t, a >> 30, &ps);
  if (!t) return -1;
  if (t[2 * i] & 1u) return -1;
  t[2 * i + 1] = 0;
  t[2 * i] = (a & ~0x1FFFFFu) | 0x9Bu;   /* P | RW | PWT | PCD | PS */
  return 0;
}

static int fch_ok(void) {
  if (sizeof(void*) != 4) {
    printf("fch: ponteiro de tamanho inesperado\n");
    return 0;
  }
  if (!mapeado(FCH) && mapeia(FCH) != 0) {
    printf("fch: 0xFED80000 nao esta mapeado e nao deu para mapear\n");
    return 0;
  }
  if (!mapeado(FCH) || !mapeado(FCH + 0x1000)) {
    printf("fch: 0xFED80000 nao esta mapeado neste boot\n");
    return 0;
  }
  return 1;
}

static void fch_bytes(const char* nome, unsigned off) {
  volatile unsigned char* p = (volatile unsigned char*)(FCH + off);
  unsigned i;
  for (i = 0; i < 256; i++) {
    if (i % 16 == 0) printf("%s %03X:", nome, i);
    printf(" %02X", (unsigned)p[i]);
    if (i % 16 == 15) printf("\n");
  }
}

static int fch_dump(void) {
  volatile unsigned* g = (volatile unsigned*)(FCH + 0x1500);
  unsigned i;
  if (!fch_ok()) return 1;
  fch_bytes("smi", 0x200);
  fch_bytes("pm", 0x300);
  fch_bytes("pm2", 0x400);
  fch_bytes("iomux", 0xD00);
  fch_bytes("misc", 0xE00);
  fch_bytes("rgpio", 0x1200);
  fch_bytes("aoac", 0x1E00);
  /* um registrador de 32 bits por pino: bit 16 nivel lido, bit 22 valor de
   * saida, bit 23 saida ligada (mesmo formato das fotos de 25/11) */
  for (i = 0; i < 256; i++) {
    if (i % 8 == 0) printf("gpio %02X:", i);
    printf(" %08X", g[i]);
    if (i % 8 == 7) printf("\n");
  }
  return 0;
}

/* _REG do H_EC (DSDT): SSD1 = nivel do GPIO 4, SSD2 = nivel do GPIO 7, nos
 * dois nibbles do byte 0x40 do EC. O ALIB (0x0C) e o BACC ficam de fora. */
static int reg_ec(void) {
  volatile unsigned* g = (volatile unsigned*)(FCH + 0x1500);
  unsigned antes, novo, depois;
  if (!fch_ok()) return 1;
  novo = ((g[4] >> 16) & 1u) | (((g[7] >> 16) & 1u) << 4);
  if (ec_read(0x40, &antes) || ec_write(0x40, novo) || ec_read(0x40, &depois)) {
    printf("ec: sem resposta\n");
    return 1;
  }
  printf("ec 40 (SSD1/SSD2): %02X -> gravou %02X -> %02X\n", antes, novo, depois);
  return 0;
}

/* ---------- PCI: os 256 bytes de configuracao de todas as funcoes ---------- */

/* mov dx, imm16 ; mov eax, imm32 ; out dx, eax ; ret */
static unsigned char cod_outl[] = { 0x66, 0xBA, 0, 0, 0xB8, 0, 0, 0, 0, 0xEF, 0xC3 };
/* mov dx, imm16 ; in eax, dx ; ret */
static unsigned char cod_inl[] = { 0x66, 0xBA, 0, 0, 0xED, 0xC3 };

static unsigned pci_once(unsigned b, unsigned d, unsigned f, unsigned off) {
  void (*o)(void) = (void (*)(void))cod_outl;
  unsigned (*i)(void) = (unsigned (*)(void))cod_inl;
  unsigned a = 0x80000000u | b << 16 | d << 11 | f << 8 | (off & 0xFCu);
  cod_outl[2] = 0xF8;
  cod_outl[3] = 0x0C;
  cod_outl[5] = (unsigned char)a;
  cod_outl[6] = (unsigned char)(a >> 8);
  cod_outl[7] = (unsigned char)(a >> 16);
  cod_outl[8] = (unsigned char)(a >> 24);
  o();
  cod_inl[2] = 0xFC;
  cod_inl[3] = 0x0C;
  return i();
}

/* O kernel tambem usa CF8/CFC: le ate duas leituras seguidas baterem. */
static unsigned pci_rd(unsigned b, unsigned d, unsigned f, unsigned off) {
  unsigned v = pci_once(b, d, f, off), w, n;
  for (n = 0; n < 8; n++) {
    w = pci_once(b, d, f, off);
    if (w == v) break;
    v = w;
  }
  return v;
}

static int pci_dump(void) {
  unsigned b, d, f, off, v, k;
  for (b = 0; b < 16; b++)
    for (d = 0; d < 32; d++)
      for (f = 0; f < 8; f++) {
        v = pci_rd(b, d, f, 0);
        if ((v & 0xFFFFu) == 0xFFFFu || v == 0) {
          if (f == 0) break;
          continue;
        }
        for (off = 0; off < 256; off += 4) {
          v = pci_rd(b, d, f, off);
          if (off % 16 == 0) printf("pci %02X:%02X.%X %03X:", b, d, f, off);
          for (k = 0; k < 4; k++) printf(" %02X", (v >> (8 * k)) & 0xFFu);
          if (off % 16 == 12) printf("\n");
        }
        if (f == 0 && !(pci_rd(b, d, 0, 0x0C) & 0x00800000u)) break;   /* uma funcao so */
      }
  return 0;
}

static unsigned hex(const char* s) {
  unsigned v = 0;
  for (; *s; s++) {
    int c = *s, d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else break;
    v = v * 16 + (unsigned)d;
  }
  return v;
}

static int igual(const char* a, const char* b) {
  for (; *a && *b; a++, b++) {
    int x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return 0;
  }
  return *a == *b;
}

int main(int argc, char** argv) {
  unsigned a, v, antes, depois;
  if (argc >= 2 && igual(argv[1], "EC")) {
    if (argc == 2) return ec_dump();
    a = hex(argv[2]);
    if (a > 0xFF) {
      printf("SOMDIAG: endereco do EC vai de 00 a FF\n");
      return 1;
    }
    if (ec_read(a, &antes)) {
      printf("ec: sem resposta\n");
      return 1;
    }
    if (argc == 3) {
      printf("ec %02X = %02X\n", a, antes);
      return 0;
    }
    v = hex(argv[3]) & 0xFFu;
    if (ec_write(a, v) || ec_read(a, &depois)) {
      printf("ec: sem resposta ao gravar\n");
      return 1;
    }
    printf("ec %02X: %02X -> gravou %02X -> %02X\n", a, antes, v, depois);
    return 0;
  }
  if (argc >= 2 && igual(argv[1], "FCH")) return fch_dump();
  if (argc >= 2 && igual(argv[1], "REG")) return reg_ec();
  if (argc >= 2 && igual(argv[1], "PCI")) return pci_dump();
  if (argc >= 2 && igual(argv[1], "Q")) {
    /* Eventos pendentes do EC (QR_EC, 0x84), como o driver de EC do Linux
     * faz ao receber o SCI. So le o numero: nao roda o _Qxx do DSDT. */
    unsigned n, st, q;
    for (n = 0; n < 16; n++) {
      st = inb(0x66);
      if (!(st & 0x20u)) {
        printf("ec estado %02X: %s\n", st, n ? "sem mais eventos" : "nenhum evento pendente");
        return 0;
      }
      if (st & 1u) (void)inb(0x62);
      if (ec_wait(2, 0)) break;
      outb(0x66, 0x84);
      if (ec_wait(1, 1)) break;
      q = inb(0x62);
      printf("ec evento _Q%02X\n", q);
      if (q == 0) return 0;
    }
    printf("ec: parou (sem resposta ou eventos demais)\n");
    return 1;
  }
  if (argc >= 4 && igual(argv[1], "SMI")) {
    /* SMI por software como o AML faz (APMD = dado em B3, APMC = comando em
     * B2). O DSDT usa C0 com dado 2 no _Q40 e C0 com dado 1 no _Q41. */
    a = hex(argv[2]) & 0xFFu;
    v = hex(argv[3]) & 0xFFu;
    outb(0xB3, v);
    outb(0xB2, a);
    printf("smi: comando %02X dado %02X enviado\n", a, v);
    return 0;
  }
  if (argc >= 2 && igual(argv[1], "FOTO")) {
    int r = ec_dump();
    r |= fch_dump();
    return pci_dump() | r;
  }
  printf("Uso: SOMDIAG EC [aa [vv]] | FCH | PCI | FOTO | REG   (hexadecimal)\n");
  return 1;
}
