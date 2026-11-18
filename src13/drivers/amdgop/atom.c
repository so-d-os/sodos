/*
 * Copyright 2008 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author: Stanislaw Skowronek
 */

/* atom.c - interpretador do AtomBIOS, portado do Linux v6.6
 * (drivers/gpu/drm/amd/amdgpu/atom.c, licenca MIT acima) para o driver da
 * RX 570 do SODOS (etapa M, dois monitores). Gerado em 2026-11-18 por um
 * script que copia o corpo do arquivo e troca so o que e do kernel Linux:
 *  - leituras da ROM com limite (fora da imagem: aborta a tabela);
 *  - jiffies -> at_ms(); udelay/mdelay/msleep -> esperas do driver;
 *  - kcalloc do WS -> pilha propria; iio em tabela estatica;
 *  - mutex fora (o driver chama de um lugar so);
 *  - laco preso: 2 s (Linux: 20), e um teto de instrucoes por chamada.
 * Incluido por amdgop.c; o host (atomtest.c) inclui direto. */
#include <stdint.h>
#include <stdbool.h>
#ifndef NULL
#define NULL ((void*)0)
#endif

typedef uint32_t u32;

#define ATOM_BIOS_MAGIC		0xAA55
#define ATOM_ATI_MAGIC_PTR	0x30
#define ATOM_ATI_MAGIC		" 761295520"
#define ATOM_ROM_TABLE_PTR	0x48

#define ATOM_ROM_MAGIC		"ATOM"
#define ATOM_ROM_MAGIC_PTR	4

#define ATOM_ROM_MSG_PTR	0x10
#define ATOM_ROM_CMD_PTR	0x1E
#define ATOM_ROM_DATA_PTR	0x20

#define ATOM_CMD_INIT		0
#define ATOM_CMD_SETSCLK	0x0A
#define ATOM_CMD_SETMCLK	0x0B
#define ATOM_CMD_SETPCLK	0x0C
#define ATOM_CMD_SPDFANCNTL	0x39

#define ATOM_DATA_FWI_PTR	0xC
#define ATOM_DATA_IIO_PTR	0x32

#define ATOM_FWI_DEFSCLK_PTR	8
#define ATOM_FWI_DEFMCLK_PTR	0xC
#define ATOM_FWI_MAXSCLK_PTR	0x24
#define ATOM_FWI_MAXMCLK_PTR	0x28

#define ATOM_CT_SIZE_PTR	0
#define ATOM_CT_WS_PTR		4
#define ATOM_CT_PS_PTR		5
#define ATOM_CT_PS_MASK		0x7F
#define ATOM_CT_CODE_PTR	6

#define ATOM_OP_CNT		127
#define ATOM_OP_EOT		91

#define ATOM_CASE_MAGIC		0x63
#define ATOM_CASE_END		0x5A5A

#define ATOM_ARG_REG		0
#define ATOM_ARG_PS		1
#define ATOM_ARG_WS		2
#define ATOM_ARG_FB		3
#define ATOM_ARG_ID		4
#define ATOM_ARG_IMM		5
#define ATOM_ARG_PLL		6
#define ATOM_ARG_MC		7

#define ATOM_SRC_DWORD		0
#define ATOM_SRC_WORD0		1
#define ATOM_SRC_WORD8		2
#define ATOM_SRC_WORD16		3
#define ATOM_SRC_BYTE0		4
#define ATOM_SRC_BYTE8		5
#define ATOM_SRC_BYTE16		6
#define ATOM_SRC_BYTE24		7

#define ATOM_WS_QUOTIENT	0x40
#define ATOM_WS_REMAINDER	0x41
#define ATOM_WS_DATAPTR		0x42
#define ATOM_WS_SHIFT		0x43
#define ATOM_WS_OR_MASK		0x44
#define ATOM_WS_AND_MASK	0x45
#define ATOM_WS_FB_WINDOW	0x46
#define ATOM_WS_ATTRIBUTES	0x47
#define ATOM_WS_REGPTR		0x48

#define ATOM_IIO_NOP		0
#define ATOM_IIO_START		1
#define ATOM_IIO_READ		2
#define ATOM_IIO_WRITE		3
#define ATOM_IIO_CLEAR		4
#define ATOM_IIO_SET		5
#define ATOM_IIO_MOVE_INDEX	6
#define ATOM_IIO_MOVE_ATTR	7
#define ATOM_IIO_MOVE_DATA	8
#define ATOM_IIO_END		9

#define ATOM_IO_MM		0
#define ATOM_IO_PCI		1
#define ATOM_IO_SYSIO		2
#define ATOM_IO_IIO		0x80

#define STRLEN_NORMAL		32
#define STRLEN_LONG		64
#define STRLEN_VERYLONG		254

#define ATOM_OP_NAMES_CNT 0
#define ATOM_TABLE_NAMES_CNT 0
#define ATOM_IO_NAMES_CNT 0
#define lower_32_bits(n) ((uint32_t)(n))
#define upper_32_bits(n) ((uint32_t)((uint64_t)(n) >> 32))
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define get_unaligned_le32(p) (*(p))
#define EINVAL 22
#define time_after(a, b) ((long)((b) - (a)) < 0)

struct card_info {
  void (*reg_write)(struct card_info *info, u32 reg, uint32_t val);
  uint32_t (*reg_read)(struct card_info *info, uint32_t reg);
  void (*mc_write)(struct card_info *info, u32 reg, uint32_t val);
  uint32_t (*mc_read)(struct card_info *info, uint32_t reg);
  void (*pll_write)(struct card_info *info, u32 reg, uint32_t val);
  uint32_t (*pll_read)(struct card_info *info, uint32_t reg);
};

struct atom_context {
  struct card_info *card;
  void *bios;
  uint32_t cmd_table, data_table;
  uint16_t *iio;
  uint16_t data_block;
  uint32_t fb_base;
  uint32_t divmul[2];
  uint16_t io_attr;
  uint16_t reg_block;
  uint8_t shift;
  int cs_equal, cs_above;
  int io_mode;
  uint32_t *scratch;
  int scratch_size_bytes;
};

/* o que o driver (ou o teste no host) liga */
static uint32_t at_tam;                      /* bytes da imagem */
static int at_fora;                          /* leu fora da imagem */
static uint32_t at_ops, at_ops_max = 2000000u;
static uint32_t at_t0, at_t_max = 3000u;     /* ms por chamada de cima */
static uint32_t (*at_relogio)(void);
static void (*at_espera_us)(uint32_t us);
static void (*at_msg)(const char* s);
static uint16_t at_iio_tab[256];
#define AT_WS_MAX 8192u
static uint32_t at_ws_pool[AT_WS_MAX];
static uint32_t at_ws_uso;
static uint32_t at_ws_falhou;

static uint32_t at_ms(void) { return at_relogio ? at_relogio() : 0; }

/* printf minimo: %d %u %x %X (com largura 0N) e %s; o resto sai como esta */
static void at_printk(const char* fmt, ...) {
  char b[160];
  uint32_t n = 0;
  __builtin_va_list ap;
  if (!at_msg) return;
  __builtin_va_start(ap, fmt);
  while (*fmt && n < sizeof b - 12u) {
    char c = *fmt++;
    if (c != '%') {
      b[n++] = c;
      continue;
    }
    {
      int larg = 0;
      while (*fmt >= '0' && *fmt <= '9') larg = larg * 10 + (*fmt++ - '0');
      c = *fmt ? *fmt++ : 0;
      if (c == 's') {
        const char* s = __builtin_va_arg(ap, const char*);
        while (s && *s && n < sizeof b - 12u) b[n++] = *s++;
      } else if (c == 'd' || c == 'u' || c == 'x' || c == 'X') {
        uint32_t v = (uint32_t)__builtin_va_arg(ap, int), base = (c == 'x' || c == 'X') ? 16u : 10u;
        char t[12];
        int k2 = 0;
        if (c == 'd' && (int32_t)v < 0) { b[n++] = '-'; v = (uint32_t)(-(int32_t)v); }
        do { t[k2++] = "0123456789ABCDEF"[v % base]; v /= base; } while (v && k2 < 11);
        while (k2 < larg && k2 < 11) t[k2++] = '0';
        while (k2) b[n++] = t[--k2];
      } else if (c) {
        b[n++] = c;
      }
    }
  }
  __builtin_va_end(ap);
  b[n] = 0;
  at_msg(b);
}

static void udelay(uint32_t us) { if (at_espera_us) at_espera_us(us); }
static void mdelay(uint32_t ms) { if (at_espera_us) at_espera_us(ms * 1000u); }
static void msleep(uint32_t ms) { mdelay(ms); }
static int drm_can_sleep(void) { return 0; }

/* WS: pilha (as chamadas se aninham e voltam na ordem inversa) */
static uint32_t* at_ws_alloc(int n) {
  uint32_t i, *p;
  if (n <= 0 || at_ws_uso + (uint32_t)n > AT_WS_MAX) {
    at_ws_falhou = 1;
    return at_ws_pool;   /* nunca 0: o corpo do Linux nao confere; a falha aborta */
  }
  p = at_ws_pool + at_ws_uso;
  for (i = 0; i < (uint32_t)n; i++) p[i] = 0;
  at_ws_uso += (uint32_t)n;
  return p;
}
static void at_ws_free(int n) {
  if (n > 0 && at_ws_uso >= (uint32_t)n && !at_ws_falhou) at_ws_uso -= (uint32_t)n;
}

static int at_estourou(void) {
  if (at_fora || at_ws_falhou) return 1;
  if (++at_ops > at_ops_max) return 1;
  if (at_relogio && at_relogio() - at_t0 > at_t_max) return 1;
  return 0;
}

/* atom-bits.h, com limite */
static inline uint8_t get_u8(void *bios, int ptr) {
  if (ptr < 0 || (uint32_t)ptr >= at_tam) {
    at_fora = 1;
    return 0;
  }
  return ((const uint8_t*)bios)[ptr];
}
#define U8(ptr) get_u8(ctx->ctx->bios, (ptr))
#define CU8(ptr) get_u8(ctx->bios, (ptr))
static inline uint16_t get_u16(void *bios, int ptr) {
  return (uint16_t)(get_u8(bios, ptr) | ((uint16_t)get_u8(bios, ptr + 1)) << 8);
}
#define U16(ptr) get_u16(ctx->ctx->bios, (ptr))
#define CU16(ptr) get_u16(ctx->bios, (ptr))
static inline uint32_t get_u32(void *bios, int ptr) {
  return get_u16(bios, ptr) | (((uint32_t)get_u16(bios, ptr + 2)) << 16);
}
#define U32(ptr) get_u32(ctx->ctx->bios, (ptr))
#define CU32(ptr) get_u32(ctx->bios, (ptr))

#define ATOM_COND_ABOVE		0
#define ATOM_COND_ABOVEOREQUAL	1
#define ATOM_COND_ALWAYS	2
#define ATOM_COND_BELOW		3
#define ATOM_COND_BELOWOREQUAL	4
#define ATOM_COND_EQUAL		5
#define ATOM_COND_NOTEQUAL	6

#define ATOM_PORT_ATI	0
#define ATOM_PORT_PCI	1
#define ATOM_PORT_SYSIO	2

#define ATOM_UNIT_MICROSEC	0
#define ATOM_UNIT_MILLISEC	1

#define PLL_INDEX	2
#define PLL_DATA	3

#define ATOM_CMD_TIMEOUT_SEC	2   /* SODOS: 2 s no mesmo laco (Linux: 20) */

typedef struct {
	struct atom_context *ctx;
	uint32_t *ps, *ws;
	int ps_shift;
	uint16_t start;
	unsigned last_jump;
	unsigned long last_jump_jiffies;
	bool abort;
} atom_exec_context;

static int amdgpu_atom_debug;
static int amdgpu_atom_execute_table_locked(struct atom_context *ctx, int index, uint32_t *params);
static int amdgpu_atom_execute_table(struct atom_context *ctx, int index, uint32_t *params);

static uint32_t atom_arg_mask[8] =
	{ 0xFFFFFFFF, 0xFFFF, 0xFFFF00, 0xFFFF0000, 0xFF, 0xFF00, 0xFF0000,
	  0xFF000000 };
static int atom_arg_shift[8] = { 0, 0, 8, 16, 0, 8, 16, 24 };

static int atom_dst_to_src[8][4] = {
	/* translate destination alignment field to the source alignment encoding */
	{0, 0, 0, 0},
	{1, 2, 3, 0},
	{1, 2, 3, 0},
	{1, 2, 3, 0},
	{4, 5, 6, 7},
	{4, 5, 6, 7},
	{4, 5, 6, 7},
	{4, 5, 6, 7},
};
static int atom_def_dst[8] = { 0, 0, 1, 2, 0, 1, 2, 3 };

static int debug_depth;
#ifdef ATOM_DEBUG
static void debug_print_spaces(int n)
{
	while (n--)
		at_printk("   ");
}

#define DEBUG(...) do if (amdgpu_atom_debug) { at_printk(KERN_DEBUG __VA_ARGS__); } while (0)
#define SDEBUG(...) do if (amdgpu_atom_debug) { at_printk(KERN_DEBUG); debug_print_spaces(debug_depth); at_printk(__VA_ARGS__); } while (0)
#else
#define DEBUG(...) do { } while (0)
#define SDEBUG(...) do { } while (0)
#endif

static uint32_t atom_iio_execute(struct atom_context *ctx, int base,
				 uint32_t index, uint32_t data)
{
	uint32_t temp = 0xCDCDCDCD;

	while (1)
		switch (CU8(base)) {
		case ATOM_IIO_NOP:
			base++;
			break;
		case ATOM_IIO_READ:
			temp = ctx->card->reg_read(ctx->card, CU16(base + 1));
			base += 3;
			break;
		case ATOM_IIO_WRITE:
			ctx->card->reg_write(ctx->card, CU16(base + 1), temp);
			base += 3;
			break;
		case ATOM_IIO_CLEAR:
			temp &=
			    ~((0xFFFFFFFF >> (32 - CU8(base + 1))) <<
			      CU8(base + 2));
			base += 3;
			break;
		case ATOM_IIO_SET:
			temp |=
			    (0xFFFFFFFF >> (32 - CU8(base + 1))) << CU8(base +
									2);
			base += 3;
			break;
		case ATOM_IIO_MOVE_INDEX:
			temp &=
			    ~((0xFFFFFFFF >> (32 - CU8(base + 1))) <<
			      CU8(base + 3));
			temp |=
			    ((index >> CU8(base + 2)) &
			     (0xFFFFFFFF >> (32 - CU8(base + 1)))) << CU8(base +
									  3);
			base += 4;
			break;
		case ATOM_IIO_MOVE_DATA:
			temp &=
			    ~((0xFFFFFFFF >> (32 - CU8(base + 1))) <<
			      CU8(base + 3));
			temp |=
			    ((data >> CU8(base + 2)) &
			     (0xFFFFFFFF >> (32 - CU8(base + 1)))) << CU8(base +
									  3);
			base += 4;
			break;
		case ATOM_IIO_MOVE_ATTR:
			temp &=
			    ~((0xFFFFFFFF >> (32 - CU8(base + 1))) <<
			      CU8(base + 3));
			temp |=
			    ((ctx->
			      io_attr >> CU8(base + 2)) & (0xFFFFFFFF >> (32 -
									  CU8
									  (base
									   +
									   1))))
			    << CU8(base + 3);
			base += 4;
			break;
		case ATOM_IIO_END:
			return temp;
		default:
			at_printk("Unknown IIO opcode\n");
			return 0;
		}
}

static uint32_t atom_get_src_int(atom_exec_context *ctx, uint8_t attr,
				 int *ptr, uint32_t *saved, int print)
{
	uint32_t idx, val = 0xCDCDCDCD, align, arg;
	struct atom_context *gctx = ctx->ctx;
	arg = attr & 7;
	align = (attr >> 3) & 7;
	switch (arg) {
	case ATOM_ARG_REG:
		idx = U16(*ptr);
		(*ptr) += 2;
		if (print)
			DEBUG("REG[0x%04X]", idx);
		idx += gctx->reg_block;
		switch (gctx->io_mode) {
		case ATOM_IO_MM:
			val = gctx->card->reg_read(gctx->card, idx);
			break;
		case ATOM_IO_PCI:
			at_printk("PCI registers are not implemented\n");
			return 0;
		case ATOM_IO_SYSIO:
			at_printk("SYSIO registers are not implemented\n");
			return 0;
		default:
			if (!(gctx->io_mode & 0x80)) {
				at_printk("Bad IO mode\n");
				return 0;
			}
			if (!gctx->iio[gctx->io_mode & 0x7F]) {
				at_printk("Undefined indirect IO read method %d\n",
					gctx->io_mode & 0x7F);
				return 0;
			}
			val =
			    atom_iio_execute(gctx,
					     gctx->iio[gctx->io_mode & 0x7F],
					     idx, 0);
		}
		break;
	case ATOM_ARG_PS:
		idx = U8(*ptr);
		(*ptr)++;
		/* get_unaligned_le32 avoids unaligned accesses from atombios
		 * tables, noticed on a DEC Alpha. */
		val = get_unaligned_le32((u32 *)&ctx->ps[idx]);
		if (print)
			DEBUG("PS[0x%02X,0x%04X]", idx, val);
		break;
	case ATOM_ARG_WS:
		idx = U8(*ptr);
		(*ptr)++;
		if (print)
			DEBUG("WS[0x%02X]", idx);
		switch (idx) {
		case ATOM_WS_QUOTIENT:
			val = gctx->divmul[0];
			break;
		case ATOM_WS_REMAINDER:
			val = gctx->divmul[1];
			break;
		case ATOM_WS_DATAPTR:
			val = gctx->data_block;
			break;
		case ATOM_WS_SHIFT:
			val = gctx->shift;
			break;
		case ATOM_WS_OR_MASK:
			val = 1 << gctx->shift;
			break;
		case ATOM_WS_AND_MASK:
			val = ~(1 << gctx->shift);
			break;
		case ATOM_WS_FB_WINDOW:
			val = gctx->fb_base;
			break;
		case ATOM_WS_ATTRIBUTES:
			val = gctx->io_attr;
			break;
		case ATOM_WS_REGPTR:
			val = gctx->reg_block;
			break;
		default:
			val = ctx->ws[idx];
		}
		break;
	case ATOM_ARG_ID:
		idx = U16(*ptr);
		(*ptr) += 2;
		if (print) {
			if (gctx->data_block)
				DEBUG("ID[0x%04X+%04X]", idx, gctx->data_block);
			else
				DEBUG("ID[0x%04X]", idx);
		}
		val = U32(idx + gctx->data_block);
		break;
	case ATOM_ARG_FB:
		idx = U8(*ptr);
		(*ptr)++;
		if ((gctx->fb_base + (idx * 4)) > gctx->scratch_size_bytes) {
			at_printk("ATOM: fb read beyond scratch region: %d vs. %d\n",
				  gctx->fb_base + (idx * 4), gctx->scratch_size_bytes);
			val = 0;
		} else
			val = gctx->scratch[(gctx->fb_base / 4) + idx];
		if (print)
			DEBUG("FB[0x%02X]", idx);
		break;
	case ATOM_ARG_IMM:
		switch (align) {
		case ATOM_SRC_DWORD:
			val = U32(*ptr);
			(*ptr) += 4;
			if (print)
				DEBUG("IMM 0x%08X\n", val);
			return val;
		case ATOM_SRC_WORD0:
		case ATOM_SRC_WORD8:
		case ATOM_SRC_WORD16:
			val = U16(*ptr);
			(*ptr) += 2;
			if (print)
				DEBUG("IMM 0x%04X\n", val);
			return val;
		case ATOM_SRC_BYTE0:
		case ATOM_SRC_BYTE8:
		case ATOM_SRC_BYTE16:
		case ATOM_SRC_BYTE24:
			val = U8(*ptr);
			(*ptr)++;
			if (print)
				DEBUG("IMM 0x%02X\n", val);
			return val;
		}
		return 0;
	case ATOM_ARG_PLL:
		idx = U8(*ptr);
		(*ptr)++;
		if (print)
			DEBUG("PLL[0x%02X]", idx);
		val = gctx->card->pll_read(gctx->card, idx);
		break;
	case ATOM_ARG_MC:
		idx = U8(*ptr);
		(*ptr)++;
		if (print)
			DEBUG("MC[0x%02X]", idx);
		val = gctx->card->mc_read(gctx->card, idx);
		break;
	}
	if (saved)
		*saved = val;
	val &= atom_arg_mask[align];
	val >>= atom_arg_shift[align];
	if (print)
		switch (align) {
		case ATOM_SRC_DWORD:
			DEBUG(".[31:0] -> 0x%08X\n", val);
			break;
		case ATOM_SRC_WORD0:
			DEBUG(".[15:0] -> 0x%04X\n", val);
			break;
		case ATOM_SRC_WORD8:
			DEBUG(".[23:8] -> 0x%04X\n", val);
			break;
		case ATOM_SRC_WORD16:
			DEBUG(".[31:16] -> 0x%04X\n", val);
			break;
		case ATOM_SRC_BYTE0:
			DEBUG(".[7:0] -> 0x%02X\n", val);
			break;
		case ATOM_SRC_BYTE8:
			DEBUG(".[15:8] -> 0x%02X\n", val);
			break;
		case ATOM_SRC_BYTE16:
			DEBUG(".[23:16] -> 0x%02X\n", val);
			break;
		case ATOM_SRC_BYTE24:
			DEBUG(".[31:24] -> 0x%02X\n", val);
			break;
		}
	return val;
}

static void atom_skip_src_int(atom_exec_context *ctx, uint8_t attr, int *ptr)
{
	uint32_t align = (attr >> 3) & 7, arg = attr & 7;
	switch (arg) {
	case ATOM_ARG_REG:
	case ATOM_ARG_ID:
		(*ptr) += 2;
		break;
	case ATOM_ARG_PLL:
	case ATOM_ARG_MC:
	case ATOM_ARG_PS:
	case ATOM_ARG_WS:
	case ATOM_ARG_FB:
		(*ptr)++;
		break;
	case ATOM_ARG_IMM:
		switch (align) {
		case ATOM_SRC_DWORD:
			(*ptr) += 4;
			return;
		case ATOM_SRC_WORD0:
		case ATOM_SRC_WORD8:
		case ATOM_SRC_WORD16:
			(*ptr) += 2;
			return;
		case ATOM_SRC_BYTE0:
		case ATOM_SRC_BYTE8:
		case ATOM_SRC_BYTE16:
		case ATOM_SRC_BYTE24:
			(*ptr)++;
			return;
		}
		return;
	}
}

static uint32_t atom_get_src(atom_exec_context *ctx, uint8_t attr, int *ptr)
{
	return atom_get_src_int(ctx, attr, ptr, NULL, 1);
}

static uint32_t atom_get_src_direct(atom_exec_context *ctx, uint8_t align, int *ptr)
{
	uint32_t val = 0xCDCDCDCD;

	switch (align) {
	case ATOM_SRC_DWORD:
		val = U32(*ptr);
		(*ptr) += 4;
		break;
	case ATOM_SRC_WORD0:
	case ATOM_SRC_WORD8:
	case ATOM_SRC_WORD16:
		val = U16(*ptr);
		(*ptr) += 2;
		break;
	case ATOM_SRC_BYTE0:
	case ATOM_SRC_BYTE8:
	case ATOM_SRC_BYTE16:
	case ATOM_SRC_BYTE24:
		val = U8(*ptr);
		(*ptr)++;
		break;
	}
	return val;
}

static uint32_t atom_get_dst(atom_exec_context *ctx, int arg, uint8_t attr,
			     int *ptr, uint32_t *saved, int print)
{
	return atom_get_src_int(ctx,
				arg | atom_dst_to_src[(attr >> 3) &
						      7][(attr >> 6) & 3] << 3,
				ptr, saved, print);
}

static void atom_skip_dst(atom_exec_context *ctx, int arg, uint8_t attr, int *ptr)
{
	atom_skip_src_int(ctx,
			  arg | atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) &
								 3] << 3, ptr);
}

static void atom_put_dst(atom_exec_context *ctx, int arg, uint8_t attr,
			 int *ptr, uint32_t val, uint32_t saved)
{
	uint32_t align =
	    atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3], old_val =
	    val, idx;
	struct atom_context *gctx = ctx->ctx;
	old_val &= atom_arg_mask[align] >> atom_arg_shift[align];
	val <<= atom_arg_shift[align];
	val &= atom_arg_mask[align];
	saved &= ~atom_arg_mask[align];
	val |= saved;
	switch (arg) {
	case ATOM_ARG_REG:
		idx = U16(*ptr);
		(*ptr) += 2;
		DEBUG("REG[0x%04X]", idx);
		idx += gctx->reg_block;
		switch (gctx->io_mode) {
		case ATOM_IO_MM:
			if (idx == 0)
				gctx->card->reg_write(gctx->card, idx,
						      val << 2);
			else
				gctx->card->reg_write(gctx->card, idx, val);
			break;
		case ATOM_IO_PCI:
			at_printk("PCI registers are not implemented\n");
			return;
		case ATOM_IO_SYSIO:
			at_printk("SYSIO registers are not implemented\n");
			return;
		default:
			if (!(gctx->io_mode & 0x80)) {
				at_printk("Bad IO mode\n");
				return;
			}
			if (!gctx->iio[gctx->io_mode & 0xFF]) {
				at_printk("Undefined indirect IO write method %d\n",
					gctx->io_mode & 0x7F);
				return;
			}
			atom_iio_execute(gctx, gctx->iio[gctx->io_mode & 0xFF],
					 idx, val);
		}
		break;
	case ATOM_ARG_PS:
		idx = U8(*ptr);
		(*ptr)++;
		DEBUG("PS[0x%02X]", idx);
		ctx->ps[idx] = cpu_to_le32(val);
		break;
	case ATOM_ARG_WS:
		idx = U8(*ptr);
		(*ptr)++;
		DEBUG("WS[0x%02X]", idx);
		switch (idx) {
		case ATOM_WS_QUOTIENT:
			gctx->divmul[0] = val;
			break;
		case ATOM_WS_REMAINDER:
			gctx->divmul[1] = val;
			break;
		case ATOM_WS_DATAPTR:
			gctx->data_block = val;
			break;
		case ATOM_WS_SHIFT:
			gctx->shift = val;
			break;
		case ATOM_WS_OR_MASK:
		case ATOM_WS_AND_MASK:
			break;
		case ATOM_WS_FB_WINDOW:
			gctx->fb_base = val;
			break;
		case ATOM_WS_ATTRIBUTES:
			gctx->io_attr = val;
			break;
		case ATOM_WS_REGPTR:
			gctx->reg_block = val;
			break;
		default:
			ctx->ws[idx] = val;
		}
		break;
	case ATOM_ARG_FB:
		idx = U8(*ptr);
		(*ptr)++;
		if ((gctx->fb_base + (idx * 4)) > gctx->scratch_size_bytes) {
			at_printk("ATOM: fb write beyond scratch region: %d vs. %d\n",
				  gctx->fb_base + (idx * 4), gctx->scratch_size_bytes);
		} else
			gctx->scratch[(gctx->fb_base / 4) + idx] = val;
		DEBUG("FB[0x%02X]", idx);
		break;
	case ATOM_ARG_PLL:
		idx = U8(*ptr);
		(*ptr)++;
		DEBUG("PLL[0x%02X]", idx);
		gctx->card->pll_write(gctx->card, idx, val);
		break;
	case ATOM_ARG_MC:
		idx = U8(*ptr);
		(*ptr)++;
		DEBUG("MC[0x%02X]", idx);
		gctx->card->mc_write(gctx->card, idx, val);
		return;
	}
	switch (align) {
	case ATOM_SRC_DWORD:
		DEBUG(".[31:0] <- 0x%08X\n", old_val);
		break;
	case ATOM_SRC_WORD0:
		DEBUG(".[15:0] <- 0x%04X\n", old_val);
		break;
	case ATOM_SRC_WORD8:
		DEBUG(".[23:8] <- 0x%04X\n", old_val);
		break;
	case ATOM_SRC_WORD16:
		DEBUG(".[31:16] <- 0x%04X\n", old_val);
		break;
	case ATOM_SRC_BYTE0:
		DEBUG(".[7:0] <- 0x%02X\n", old_val);
		break;
	case ATOM_SRC_BYTE8:
		DEBUG(".[15:8] <- 0x%02X\n", old_val);
		break;
	case ATOM_SRC_BYTE16:
		DEBUG(".[23:16] <- 0x%02X\n", old_val);
		break;
	case ATOM_SRC_BYTE24:
		DEBUG(".[31:24] <- 0x%02X\n", old_val);
		break;
	}
}

static void atom_op_add(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst += src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_and(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst &= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_beep(atom_exec_context *ctx, int *ptr, int arg)
{
	at_printk("ATOM BIOS beeped!\n");
}

static void atom_op_calltable(atom_exec_context *ctx, int *ptr, int arg)
{
	int idx = U8((*ptr)++);
	int r = 0;

	if (idx < ATOM_TABLE_NAMES_CNT)
		SDEBUG("   table: %d (%s)\n", idx, atom_table_names[idx]);
	else
		SDEBUG("   table: %d\n", idx);
	if (U16(ctx->ctx->cmd_table + 4 + 2 * idx))
		r = amdgpu_atom_execute_table_locked(ctx->ctx, idx, ctx->ps + ctx->ps_shift);
	if (r) {
		ctx->abort = true;
	}
}

static void atom_op_clear(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t saved;
	int dptr = *ptr;
	attr &= 0x38;
	attr |= atom_def_dst[attr >> 3] << 6;
	atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, 0, saved);
}

static void atom_op_compare(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	ctx->ctx->cs_equal = (dst == src);
	ctx->ctx->cs_above = (dst > src);
	SDEBUG("   result: %s %s\n", ctx->ctx->cs_equal ? "EQ" : "NE",
	       ctx->ctx->cs_above ? "GT" : "LE");
}

static void atom_op_delay(atom_exec_context *ctx, int *ptr, int arg)
{
	unsigned count = U8((*ptr)++);
	SDEBUG("   count: %d\n", count);
	if (arg == ATOM_UNIT_MICROSEC)
		udelay(count);
	else if (!drm_can_sleep())
		mdelay(count);
	else
		msleep(count);
}

static void atom_op_div(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	if (src != 0) {
		ctx->ctx->divmul[0] = dst / src;
		ctx->ctx->divmul[1] = dst % src;
	} else {
		ctx->ctx->divmul[0] = 0;
		ctx->ctx->divmul[1] = 0;
	}
}

static void atom_op_div32(atom_exec_context *ctx, int *ptr, int arg)
{
	uint64_t val64;
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	if (src != 0) {
		val64 = dst;
		val64 |= ((uint64_t)ctx->ctx->divmul[1]) << 32;
		val64 /= src;
		ctx->ctx->divmul[0] = lower_32_bits(val64);
		ctx->ctx->divmul[1] = upper_32_bits(val64);
	} else {
		ctx->ctx->divmul[0] = 0;
		ctx->ctx->divmul[1] = 0;
	}
}

static void atom_op_eot(atom_exec_context *ctx, int *ptr, int arg)
{
	/* functionally, a nop */
}

static void atom_op_jump(atom_exec_context *ctx, int *ptr, int arg)
{
	int execute = 0, target = U16(*ptr);
	unsigned long cjiffies;

	(*ptr) += 2;
	switch (arg) {
	case ATOM_COND_ABOVE:
		execute = ctx->ctx->cs_above;
		break;
	case ATOM_COND_ABOVEOREQUAL:
		execute = ctx->ctx->cs_above || ctx->ctx->cs_equal;
		break;
	case ATOM_COND_ALWAYS:
		execute = 1;
		break;
	case ATOM_COND_BELOW:
		execute = !(ctx->ctx->cs_above || ctx->ctx->cs_equal);
		break;
	case ATOM_COND_BELOWOREQUAL:
		execute = !ctx->ctx->cs_above;
		break;
	case ATOM_COND_EQUAL:
		execute = ctx->ctx->cs_equal;
		break;
	case ATOM_COND_NOTEQUAL:
		execute = !ctx->ctx->cs_equal;
		break;
	}
	if (arg != ATOM_COND_ALWAYS)
		SDEBUG("   taken: %s\n", str_yes_no(execute));
	SDEBUG("   target: 0x%04X\n", target);
	if (execute) {
		if (ctx->last_jump == (ctx->start + target)) {
			cjiffies = at_ms();
			if (time_after(cjiffies, ctx->last_jump_jiffies)) {
				cjiffies -= ctx->last_jump_jiffies;
				if (((cjiffies) > ATOM_CMD_TIMEOUT_SEC*1000)) {
					at_printk("atombios stuck in loop for more than %dsecs aborting\n",
						  ATOM_CMD_TIMEOUT_SEC);
					ctx->abort = true;
				}
			} else {
				/* at_ms() wrap around we will just wait a little longer */
				ctx->last_jump_jiffies = at_ms();
			}
		} else {
			ctx->last_jump = ctx->start + target;
			ctx->last_jump_jiffies = at_ms();
		}
		*ptr = ctx->start + target;
	}
}

static void atom_op_mask(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, mask, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	mask = atom_get_src_direct(ctx, ((attr >> 3) & 7), ptr);
	SDEBUG("   mask: 0x%08x", mask);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst &= mask;
	dst |= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_move(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t src, saved;
	int dptr = *ptr;
	if (((attr >> 3) & 7) != ATOM_SRC_DWORD)
		atom_get_dst(ctx, arg, attr, ptr, &saved, 0);
	else {
		atom_skip_dst(ctx, arg, attr, ptr);
		saved = 0xCDCDCDCD;
	}
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, src, saved);
}

static void atom_op_mul(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	ctx->ctx->divmul[0] = dst * src;
}

static void atom_op_mul32(atom_exec_context *ctx, int *ptr, int arg)
{
	uint64_t val64;
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	val64 = (uint64_t)dst * (uint64_t)src;
	ctx->ctx->divmul[0] = lower_32_bits(val64);
	ctx->ctx->divmul[1] = upper_32_bits(val64);
}

static void atom_op_nop(atom_exec_context *ctx, int *ptr, int arg)
{
	/* nothing */
}

static void atom_op_or(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst |= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_postcard(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t val = U8((*ptr)++);
	SDEBUG("POST card output: 0x%02X\n", val);
}

static void atom_op_repeat(atom_exec_context *ctx, int *ptr, int arg)
{
	at_printk("unimplemented!\n");
}

static void atom_op_restorereg(atom_exec_context *ctx, int *ptr, int arg)
{
	at_printk("unimplemented!\n");
}

static void atom_op_savereg(atom_exec_context *ctx, int *ptr, int arg)
{
	at_printk("unimplemented!\n");
}

static void atom_op_setdatablock(atom_exec_context *ctx, int *ptr, int arg)
{
	int idx = U8(*ptr);
	(*ptr)++;
	SDEBUG("   block: %d\n", idx);
	if (!idx)
		ctx->ctx->data_block = 0;
	else if (idx == 255)
		ctx->ctx->data_block = ctx->start;
	else
		ctx->ctx->data_block = U16(ctx->ctx->data_table + 4 + 2 * idx);
	SDEBUG("   base: 0x%04X\n", ctx->ctx->data_block);
}

static void atom_op_setfbbase(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	SDEBUG("   fb_base: ");
	ctx->ctx->fb_base = atom_get_src(ctx, attr, ptr);
}

static void atom_op_setport(atom_exec_context *ctx, int *ptr, int arg)
{
	int port;
	switch (arg) {
	case ATOM_PORT_ATI:
		port = U16(*ptr);
		if (port < ATOM_IO_NAMES_CNT)
			SDEBUG("   port: %d (%s)\n", port, atom_io_names[port]);
		else
			SDEBUG("   port: %d\n", port);
		if (!port)
			ctx->ctx->io_mode = ATOM_IO_MM;
		else
			ctx->ctx->io_mode = ATOM_IO_IIO | port;
		(*ptr) += 2;
		break;
	case ATOM_PORT_PCI:
		ctx->ctx->io_mode = ATOM_IO_PCI;
		(*ptr)++;
		break;
	case ATOM_PORT_SYSIO:
		ctx->ctx->io_mode = ATOM_IO_SYSIO;
		(*ptr)++;
		break;
	}
}

static void atom_op_setregblock(atom_exec_context *ctx, int *ptr, int arg)
{
	ctx->ctx->reg_block = U16(*ptr);
	(*ptr) += 2;
	SDEBUG("   base: 0x%04X\n", ctx->ctx->reg_block);
}

static void atom_op_shift_left(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++), shift;
	uint32_t saved, dst;
	int dptr = *ptr;
	attr &= 0x38;
	attr |= atom_def_dst[attr >> 3] << 6;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	shift = atom_get_src_direct(ctx, ATOM_SRC_BYTE0, ptr);
	SDEBUG("   shift: %d\n", shift);
	dst <<= shift;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shift_right(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++), shift;
	uint32_t saved, dst;
	int dptr = *ptr;
	attr &= 0x38;
	attr |= atom_def_dst[attr >> 3] << 6;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	shift = atom_get_src_direct(ctx, ATOM_SRC_BYTE0, ptr);
	SDEBUG("   shift: %d\n", shift);
	dst >>= shift;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shl(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++), shift;
	uint32_t saved, dst;
	int dptr = *ptr;
	uint32_t dst_align = atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	/* op needs to full dst value */
	dst = saved;
	shift = atom_get_src(ctx, attr, ptr);
	SDEBUG("   shift: %d\n", shift);
	dst <<= shift;
	dst &= atom_arg_mask[dst_align];
	dst >>= atom_arg_shift[dst_align];
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_shr(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++), shift;
	uint32_t saved, dst;
	int dptr = *ptr;
	uint32_t dst_align = atom_dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	/* op needs to full dst value */
	dst = saved;
	shift = atom_get_src(ctx, attr, ptr);
	SDEBUG("   shift: %d\n", shift);
	dst >>= shift;
	dst &= atom_arg_mask[dst_align];
	dst >>= atom_arg_shift[dst_align];
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_sub(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst -= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_switch(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t src, val, target;
	SDEBUG("   switch: ");
	src = atom_get_src(ctx, attr, ptr);
	while (U16(*ptr) != ATOM_CASE_END)
		if (U8(*ptr) == ATOM_CASE_MAGIC) {
			(*ptr)++;
			SDEBUG("   case: ");
			val =
			    atom_get_src(ctx, (attr & 0x38) | ATOM_ARG_IMM,
					 ptr);
			target = U16(*ptr);
			if (val == src) {
				SDEBUG("   target: %04X\n", target);
				*ptr = ctx->start + target;
				return;
			}
			(*ptr) += 2;
		} else {
			at_printk("Bad case\n");
			return;
		}
	(*ptr) += 2;
}

static void atom_op_test(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src;
	SDEBUG("   src1: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, NULL, 1);
	SDEBUG("   src2: ");
	src = atom_get_src(ctx, attr, ptr);
	ctx->ctx->cs_equal = ((dst & src) == 0);
	SDEBUG("   result: %s\n", ctx->ctx->cs_equal ? "EQ" : "NE");
}

static void atom_op_xor(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t attr = U8((*ptr)++);
	uint32_t dst, src, saved;
	int dptr = *ptr;
	SDEBUG("   dst: ");
	dst = atom_get_dst(ctx, arg, attr, ptr, &saved, 1);
	SDEBUG("   src: ");
	src = atom_get_src(ctx, attr, ptr);
	dst ^= src;
	SDEBUG("   dst: ");
	atom_put_dst(ctx, arg, attr, &dptr, dst, saved);
}

static void atom_op_debug(atom_exec_context *ctx, int *ptr, int arg)
{
	uint8_t val = U8((*ptr)++);
	SDEBUG("DEBUG output: 0x%02X\n", val);
}

static void atom_op_processds(atom_exec_context *ctx, int *ptr, int arg)
{
	uint16_t val = U16(*ptr);
	(*ptr) += val + 2;
	SDEBUG("PROCESSDS output: 0x%02X\n", val);
}

static struct {
	void (*func) (atom_exec_context *, int *, int);
	int arg;
} opcode_table[ATOM_OP_CNT] = {
	{
	NULL, 0}, {
	atom_op_move, ATOM_ARG_REG}, {
	atom_op_move, ATOM_ARG_PS}, {
	atom_op_move, ATOM_ARG_WS}, {
	atom_op_move, ATOM_ARG_FB}, {
	atom_op_move, ATOM_ARG_PLL}, {
	atom_op_move, ATOM_ARG_MC}, {
	atom_op_and, ATOM_ARG_REG}, {
	atom_op_and, ATOM_ARG_PS}, {
	atom_op_and, ATOM_ARG_WS}, {
	atom_op_and, ATOM_ARG_FB}, {
	atom_op_and, ATOM_ARG_PLL}, {
	atom_op_and, ATOM_ARG_MC}, {
	atom_op_or, ATOM_ARG_REG}, {
	atom_op_or, ATOM_ARG_PS}, {
	atom_op_or, ATOM_ARG_WS}, {
	atom_op_or, ATOM_ARG_FB}, {
	atom_op_or, ATOM_ARG_PLL}, {
	atom_op_or, ATOM_ARG_MC}, {
	atom_op_shift_left, ATOM_ARG_REG}, {
	atom_op_shift_left, ATOM_ARG_PS}, {
	atom_op_shift_left, ATOM_ARG_WS}, {
	atom_op_shift_left, ATOM_ARG_FB}, {
	atom_op_shift_left, ATOM_ARG_PLL}, {
	atom_op_shift_left, ATOM_ARG_MC}, {
	atom_op_shift_right, ATOM_ARG_REG}, {
	atom_op_shift_right, ATOM_ARG_PS}, {
	atom_op_shift_right, ATOM_ARG_WS}, {
	atom_op_shift_right, ATOM_ARG_FB}, {
	atom_op_shift_right, ATOM_ARG_PLL}, {
	atom_op_shift_right, ATOM_ARG_MC}, {
	atom_op_mul, ATOM_ARG_REG}, {
	atom_op_mul, ATOM_ARG_PS}, {
	atom_op_mul, ATOM_ARG_WS}, {
	atom_op_mul, ATOM_ARG_FB}, {
	atom_op_mul, ATOM_ARG_PLL}, {
	atom_op_mul, ATOM_ARG_MC}, {
	atom_op_div, ATOM_ARG_REG}, {
	atom_op_div, ATOM_ARG_PS}, {
	atom_op_div, ATOM_ARG_WS}, {
	atom_op_div, ATOM_ARG_FB}, {
	atom_op_div, ATOM_ARG_PLL}, {
	atom_op_div, ATOM_ARG_MC}, {
	atom_op_add, ATOM_ARG_REG}, {
	atom_op_add, ATOM_ARG_PS}, {
	atom_op_add, ATOM_ARG_WS}, {
	atom_op_add, ATOM_ARG_FB}, {
	atom_op_add, ATOM_ARG_PLL}, {
	atom_op_add, ATOM_ARG_MC}, {
	atom_op_sub, ATOM_ARG_REG}, {
	atom_op_sub, ATOM_ARG_PS}, {
	atom_op_sub, ATOM_ARG_WS}, {
	atom_op_sub, ATOM_ARG_FB}, {
	atom_op_sub, ATOM_ARG_PLL}, {
	atom_op_sub, ATOM_ARG_MC}, {
	atom_op_setport, ATOM_PORT_ATI}, {
	atom_op_setport, ATOM_PORT_PCI}, {
	atom_op_setport, ATOM_PORT_SYSIO}, {
	atom_op_setregblock, 0}, {
	atom_op_setfbbase, 0}, {
	atom_op_compare, ATOM_ARG_REG}, {
	atom_op_compare, ATOM_ARG_PS}, {
	atom_op_compare, ATOM_ARG_WS}, {
	atom_op_compare, ATOM_ARG_FB}, {
	atom_op_compare, ATOM_ARG_PLL}, {
	atom_op_compare, ATOM_ARG_MC}, {
	atom_op_switch, 0}, {
	atom_op_jump, ATOM_COND_ALWAYS}, {
	atom_op_jump, ATOM_COND_EQUAL}, {
	atom_op_jump, ATOM_COND_BELOW}, {
	atom_op_jump, ATOM_COND_ABOVE}, {
	atom_op_jump, ATOM_COND_BELOWOREQUAL}, {
	atom_op_jump, ATOM_COND_ABOVEOREQUAL}, {
	atom_op_jump, ATOM_COND_NOTEQUAL}, {
	atom_op_test, ATOM_ARG_REG}, {
	atom_op_test, ATOM_ARG_PS}, {
	atom_op_test, ATOM_ARG_WS}, {
	atom_op_test, ATOM_ARG_FB}, {
	atom_op_test, ATOM_ARG_PLL}, {
	atom_op_test, ATOM_ARG_MC}, {
	atom_op_delay, ATOM_UNIT_MILLISEC}, {
	atom_op_delay, ATOM_UNIT_MICROSEC}, {
	atom_op_calltable, 0}, {
	atom_op_repeat, 0}, {
	atom_op_clear, ATOM_ARG_REG}, {
	atom_op_clear, ATOM_ARG_PS}, {
	atom_op_clear, ATOM_ARG_WS}, {
	atom_op_clear, ATOM_ARG_FB}, {
	atom_op_clear, ATOM_ARG_PLL}, {
	atom_op_clear, ATOM_ARG_MC}, {
	atom_op_nop, 0}, {
	atom_op_eot, 0}, {
	atom_op_mask, ATOM_ARG_REG}, {
	atom_op_mask, ATOM_ARG_PS}, {
	atom_op_mask, ATOM_ARG_WS}, {
	atom_op_mask, ATOM_ARG_FB}, {
	atom_op_mask, ATOM_ARG_PLL}, {
	atom_op_mask, ATOM_ARG_MC}, {
	atom_op_postcard, 0}, {
	atom_op_beep, 0}, {
	atom_op_savereg, 0}, {
	atom_op_restorereg, 0}, {
	atom_op_setdatablock, 0}, {
	atom_op_xor, ATOM_ARG_REG}, {
	atom_op_xor, ATOM_ARG_PS}, {
	atom_op_xor, ATOM_ARG_WS}, {
	atom_op_xor, ATOM_ARG_FB}, {
	atom_op_xor, ATOM_ARG_PLL}, {
	atom_op_xor, ATOM_ARG_MC}, {
	atom_op_shl, ATOM_ARG_REG}, {
	atom_op_shl, ATOM_ARG_PS}, {
	atom_op_shl, ATOM_ARG_WS}, {
	atom_op_shl, ATOM_ARG_FB}, {
	atom_op_shl, ATOM_ARG_PLL}, {
	atom_op_shl, ATOM_ARG_MC}, {
	atom_op_shr, ATOM_ARG_REG}, {
	atom_op_shr, ATOM_ARG_PS}, {
	atom_op_shr, ATOM_ARG_WS}, {
	atom_op_shr, ATOM_ARG_FB}, {
	atom_op_shr, ATOM_ARG_PLL}, {
	atom_op_shr, ATOM_ARG_MC}, {
	atom_op_debug, 0}, {
	atom_op_processds, 0}, {
	atom_op_mul32, ATOM_ARG_PS}, {
	atom_op_mul32, ATOM_ARG_WS}, {
	atom_op_div32, ATOM_ARG_PS}, {
	atom_op_div32, ATOM_ARG_WS},
};

static int amdgpu_atom_execute_table_locked(struct atom_context *ctx, int index, uint32_t *params)
{
	int base = CU16(ctx->cmd_table + 4 + 2 * index);
	int len, ws, ps, ptr;
	unsigned char op;
	atom_exec_context ectx;
	int ret = 0;

	if (!base)
		return -EINVAL;

	len = CU16(base + ATOM_CT_SIZE_PTR);
	ws = CU8(base + ATOM_CT_WS_PTR);
	ps = CU8(base + ATOM_CT_PS_PTR) & ATOM_CT_PS_MASK;
	ptr = base + ATOM_CT_CODE_PTR;

	SDEBUG(">> execute %04X (len %d, WS %d, PS %d)\n", base, len, ws, ps);

	ectx.ctx = ctx;
	ectx.ps_shift = ps / 4;
	ectx.start = base;
	ectx.ps = params;
	ectx.abort = false;
	ectx.last_jump = 0;
	if (ws)
		ectx.ws = at_ws_alloc(ws);
	else
		ectx.ws = NULL;

	debug_depth++;
	while (1) {
		op = CU8(ptr++);
		if (op < ATOM_OP_NAMES_CNT)
			SDEBUG("%s @ 0x%04X\n", atom_op_names[op], ptr - 1);
		else
			SDEBUG("[%d] @ 0x%04X\n", op, ptr - 1);
		if (at_estourou())
			ectx.abort = true;
		if (ectx.abort) {
			at_printk("atombios stuck executing %04X (len %d, WS %d, PS %d) @ 0x%04X\n",
				base, len, ws, ps, ptr - 1);
			ret = -EINVAL;
			goto free;
		}

		if (op < ATOM_OP_CNT && op > 0)
			opcode_table[op].func(&ectx, &ptr,
					      opcode_table[op].arg);
		else
			break;

		if (op == ATOM_OP_EOT)
			break;
	}
	debug_depth--;
	SDEBUG("<<\n");

free:
	if (ws)
		at_ws_free(ws);
	return ret;
}

static int amdgpu_atom_execute_table(struct atom_context *ctx, int index, uint32_t *params)
{
	int r;

	
	/* reset data block */
	ctx->data_block = 0;
	/* reset reg block */
	ctx->reg_block = 0;
	/* reset fb window */
	ctx->fb_base = 0;
	/* reset io mode */
	ctx->io_mode = ATOM_IO_MM;
	/* reset divmul */
	ctx->divmul[0] = 0;
	ctx->divmul[1] = 0;
	r = amdgpu_atom_execute_table_locked(ctx, index, params);
	
	return r;
}

static int atom_iio_len[] = { 1, 2, 3, 3, 3, 3, 4, 4, 4, 3 };

static void atom_index_iio(struct atom_context *ctx, int base)
{
	ctx->iio = at_iio_tab;
	if (!ctx->iio)
		return;
	while (CU8(base) == ATOM_IIO_START) {
		ctx->iio[CU8(base + 1)] = base + 2;
		base += 2;
		while (CU8(base) != ATOM_IIO_END)
			base += atom_iio_len[CU8(base)];
		base += 3;
	}
}


/* ---------- interface do SODOS ---------- */
static struct atom_context at_ctx;
static struct card_info at_card;

/* 0 = imagem com as assinaturas e tabelas. scratch: 20 KB, como o Linux
 * quando o VBIOS nao pede tamanho (amdgpu_atombios_allocate_fb_scratch). */
static int at_ini(void* bios, uint32_t tam, uint32_t* scratch, int scratch_bytes,
                  uint32_t (*rr)(struct card_info*, uint32_t),
                  void (*wr)(struct card_info*, u32, uint32_t)) {
  struct atom_context* ctx = &at_ctx;
  uint32_t base, i;
  at_tam = tam;
  at_fora = 0;
  ctx->bios = bios;
  if (CU16(0) != ATOM_BIOS_MAGIC) return -1;
  base = CU16(ATOM_ROM_TABLE_PTR);
  for (i = 0; i < 4; i++)
    if (CU8(base + ATOM_ROM_MAGIC_PTR + i) != (uint8_t)ATOM_ROM_MAGIC[i]) return -2;
  ctx->cmd_table = CU16(base + ATOM_ROM_CMD_PTR);
  ctx->data_table = CU16(base + ATOM_ROM_DATA_PTR);
  for (i = 0; i < 256; i++) at_iio_tab[i] = 0;
  atom_index_iio(ctx, CU16(ctx->data_table + ATOM_DATA_IIO_PTR) + 4);
  at_card.reg_read = rr;
  at_card.reg_write = wr;
  ctx->card = &at_card;
  ctx->scratch = scratch;
  ctx->scratch_size_bytes = scratch_bytes;
  ctx->shift = 0;
  ctx->io_attr = 0;
  ctx->cs_equal = ctx->cs_above = 0;
  return at_fora ? -3 : 0;
}

/* tamanho (bytes) e revisao de uma tabela de comandos; 0 = nao ha */
static uint32_t at_tabela(int idx, uint8_t* frev, uint8_t* crev) {
  struct atom_context* ctx = &at_ctx;
  uint32_t o = CU16(ctx->cmd_table + 4 + 2 * idx);
  if (!o) return 0;
  if (frev) *frev = CU8(o + 2);
  if (crev) *crev = CU8(o + 3);
  return CU16(o);
}

/* Roda a tabela idx com os parametros (ate 256 bytes; devolvidos com o que a
 * tabela escreveu). 0 = ok; -1 = nao ha; -2 = abortou (fora da imagem, laco
 * preso, tempo ou instrucoes demais). */
static int at_roda(int idx, void* params, uint32_t bytes) {
  static uint32_t ps[2048];   /* o Linux nao limita o PS: indice de 8 bits + desvios das chamadas */
  uint32_t i;
  uint8_t* p = (uint8_t*)params;
  int r;
  if (!at_tabela(idx, 0, 0)) return -1;
  for (i = 0; i < 2048u; i++) ps[i] = 0;
  if (bytes > 256u) bytes = 256u;
  for (i = 0; i < bytes; i++) ((uint8_t*)ps)[i] = p[i];
  at_fora = 0;
  at_ops = 0;
  at_ws_uso = 0;
  at_ws_falhou = 0;
  at_t0 = at_ms();
  r = amdgpu_atom_execute_table(&at_ctx, idx, ps);
  for (i = 0; i < bytes; i++) p[i] = ((uint8_t*)ps)[i];
  return r ? -2 : 0;
}
