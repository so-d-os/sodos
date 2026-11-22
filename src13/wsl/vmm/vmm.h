/* vmm.h - Maquina virtual AMD-V (SVM) do WSL.EXE: roda o kernel Linux do
 * WSL2 com o ext4.vhdx do Ubuntu como disco.
 *
 * O SODOS roda tudo em ring 0 com memoria em identidade, entao o proprio
 * programa executa VMRUN. A VM e um PC ISA bem simples, para nao precisar de
 * ACPI nem de APIC (o kernel sobe com nolapic acpi=off):
 *   - 8259 PIC (mestre/escravo), 8254 PIT (tick do Linux e calibracao do TSC)
 *   - 16550 UART em 0x3F8 (IRQ 4): o console (ttyS0)
 *   - CMOS RTC (hora)
 *   - PCI tipo 1: ponte 00:00.0 e virtio-blk moderno 00:01.0 (BAR0 MMIO, INTx
 *     IRQ 5). O kernel do WSL nao tem virtio-mmio por linha de comando, mas
 *     tem VIRTIO_PCI.
 * Disco: le do vhdx (via NTFS do SSD); escritas ficam numa camada em RAM
 * (copy-on-write) e somem quando a VM termina: o disco do Windows nunca e
 * gravado. */
#ifndef WSL_VMM_H
#define WSL_VMM_H
#include <stdint.h>
#include "../wslio.h"

/* ---------- configuracao ---------- */
typedef struct {
  uint32_t ram_mb;       /* RAM da VM */
  uint32_t cow_mb;       /* teto da camada de escrita em RAM */
  const uint8_t* kernel; /* bzImage */
  uint32_t kernel_len;
  const char* cmdline;
  const uint8_t* initrd; /* initrd (0 = sem); vai para o topo da RAM da VM */
  uint32_t initrd_len;
  wsl_io disk;           /* disco virtual (vhdx ou a imagem propria D:\WSL\DEBIAN.IMG) */
  int cpu;               /* CPU do SODOS dedicada a VM (-1 = ultima) */
  uint32_t timeout_s;    /* 0 = sem limite */
  const char* input;     /* digitado no console depois do boot (pode ser 0) */
  const char* input_after; /* espera este texto na saida antes de digitar */
  int verbose;
  int net;               /* 1 = placa virtio-net com NAT (nat_init ja ligou) */
  struct wslbox* box;    /* WSL=1: caixa de comandos (segunda serial) */
  int service;           /* WSL=1: sai quando SET WSL=0; ao fundo nao prende a CPU0 */
} vmm_config;

/* Checa AMD-V. 0 ok; senao preenche why. */
int vmm_check(char* why, int max);
/* Sobe a VM e fica no console ate ela parar. Codigo: 0 desligou, 1 erro. */
int vmm_run(const vmm_config* cfg);

/* ---------- interno ---------- */

/* Registradores gerais na ordem da codificacao x86 (0=rax ... 15=r15). rax e
 * rsp vivem no VMCB; o resto fica aqui (svm.S salva/restaura). */
struct vcpu_regs {
  uint64_t gpr[16];
  uint64_t vmcb_pa;      /* +128 */
  uint64_t host_save_pa; /* +136 */
  void* guest_fpu;       /* +144 */
  void* host_fpu;        /* +152 */
  uint64_t guest_xcr0;   /* +160 */
  uint64_t host_xcr0;    /* +168 */
  uint64_t use_xsave;    /* +176 */
};
void svm_enter(struct vcpu_regs* r);

/* Dispositivos (dev.c) */
struct vm;
void dev_init(struct vm* vm);
/* Liga a placa virtio-net ao NAT (nat_init ja deu certo). */
void dev_net_enable(struct vm* vm);
/* Porta de I/O. size 1/2/4. */
uint32_t dev_in(struct vm* vm, uint16_t port, int size);
void dev_out(struct vm* vm, uint16_t port, int size, uint32_t val);
/* MMIO fora da RAM. 1 tratado. */
int dev_mmio(struct vm* vm, uint64_t gpa, int size, int write, uint64_t* val);
/* Relogios e entrada; chamada a cada saida da VM. */
void dev_poll(struct vm* vm, uint64_t tsc);
/* TSC do proximo evento dos dispositivos (~0 = nenhum). */
uint64_t dev_next_event(struct vm* vm);
/* PIC: proximo vetor a injetar (-1 nenhum) e confirmacao da entrega. */
int pic_pending_vector(struct vm* vm);
void pic_ack(struct vm* vm, int vector);
void pic_set_irq(struct vm* vm, int irq, int level);

#define UART_RING 65536
struct vm {
  /* memoria */
  uint8_t* ram;   /* host (== fisico) */
  uint64_t ram_size;
  uint64_t tsc_hz;
  /* SVM */
  uint8_t* vmcb;
  struct vcpu_regs regs;
  int stop;       /* 1 = parar (pedido ou desligou) */
  int exit_code;
  char stop_why[96];
  int verbose;
  /* console: saida (vcpu -> principal) e entrada (principal -> vcpu) */
  volatile uint32_t tx_head, tx_tail;
  uint8_t tx[UART_RING];
  volatile uint32_t rx_head, rx_tail;
  uint8_t rx[4096];
  /* disco */
  wsl_io disk;
  uint64_t disk_bytes;
  uint32_t cow_max_chunks;
  void* dev; /* estado dos dispositivos (dev.c) */
};

static inline uint64_t rdtsc_(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

/* RAM da VM: ponteiro do host para gpa (0 se fora). */
static inline uint8_t* gpa_ptr(struct vm* vm, uint64_t gpa, uint64_t len) {
  if (gpa >= vm->ram_size || len > vm->ram_size - gpa) return 0;
  return vm->ram + gpa;
}

void vm_log(struct vm* vm, const char* fmt, ...);

/* Segmento de uma cadeia do virtio (dev.c) */
struct seg {
  uint8_t* p;
  uint32_t len;
  int w;
};
/* Servidor 9P2000.L (p9.c): as unidades do SODOS no Linux. */
int p9_init(void);
uint32_t p9_request(const struct seg* sg, int n, uint8_t** resp);

/* Canal de comandos (chan.c): segunda serial (0x2F8, IRQ 3) <-> wslbox. */
struct wslbox;
void chan_attach(struct wslbox* box);
void chan_poll(struct vm* vm);
int chan_pending(void);
uint8_t chan_uart_in(struct vm* vm, int r);
void chan_uart_out(struct vm* vm, int r, uint8_t v);

#endif
