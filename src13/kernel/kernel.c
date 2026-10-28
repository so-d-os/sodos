// kernel.c - minimal 32-bit kernel with a tiny shell and a few commands
#include <stdint.h>
#include <stddef.h>
#include "disk.h"
#include "bootinfo.h"
#include "idt.h"
#include "pic.h"
#include "trace.h"
#include "serial.h"
#include "shell.h"
#include "drives.h"

int strcmp(const char* a, const char* b); /* freestanding; definido mais abaixo */
static int stricmp(const char* a, const char* b);

// Declaração de funções de fat.c
void fat_set_partition_offset(uint32_t offset);
void fat_set_drive(uint32_t drive);
int fat_mount(void);
int fat_read_file(const char* filename, void* buffer, uint32_t max_size);
int fat_list_files(char* output, int max_output);
int fat_write_file(const char* filename, const void* data, uint32_t size);
struct fat_dir_entry* fat_find_file(const char* filename);
int fat_get_space_info(uint32_t* total_bytes, uint32_t* free_bytes);

// Declaração de funções de partition.c
int create_partition(uint32_t drive, uint8_t partition_num, uint32_t lba_start, uint32_t sector_count, uint8_t type);
int list_partitions(uint32_t drive, char* output, int max_output);
int get_partition_info(uint32_t drive, uint8_t partition_num, uint32_t* lba_start, uint32_t* sector_count, uint8_t* type);

// Declaração de funções de fat_format.c
int format_fat16(uint32_t drive, uint32_t lba_start, uint32_t total_sectors, const char* volume_label);

#include "visual.h"
#include "drives.h"
#include "mm.h"
#include "net.h"
#include "httpd.h"
#include "module.h"

const struct executable_entry* find_executable(const char* name) {
  for (int i = 0; i < executable_count; i++) {
    if (strcmp(executables[i].name, name) == 0)
      return &executables[i];
  }
  return 0;
}

// Sistema de arquivos em memória (compartilhado com editor)
struct mem_file mem_files[16];
int mem_file_count = 0;

struct mem_file* find_mem_file(const char* name) {
  for (int i = 0; i < mem_file_count; i++) {
    if (name_eq(mem_files[i].name, name)) {
      return &mem_files[i];
    }
  }
  return 0;
}

// Cria novo arquivo em memória
static struct mem_file* create_mem_file(const char* name) {
  struct mem_file* f;
  char* buf;
  int i = 0;
  if (mem_file_count >= 16) return 0;
  buf = (char*)kmalloc(1024);
  if (!buf) return 0;

  f = &mem_files[mem_file_count++];
  while (name[i] != '\0' && i < 31) {
    f->name[i] = name[i];
    i++;
  }
  f->name[i] = '\0';
  f->capacity = 1024;
  f->content = buf;
  f->size = 0;
  f->content[0] = '\0';
  return f;
}

// Escreve string em arquivo em memória (substitui conteúdo)
int write_to_mem_file(const char* filename, const char* content) {
  struct mem_file* file = find_mem_file(filename);
  if (!file) {
    file = create_mem_file(filename);
    if (!file) return -1;
  }
  
  // Limpa conteúdo anterior
  file->size = 0;
  
  // Escreve novo conteúdo
  int i = 0;
  while (content[i] != '\0' && file->size < file->capacity - 1) {
    file->content[file->size++] = content[i++];
  }
  file->content[file->size] = '\0';
  
  return 0;
}

int write_mem_bytes(const char* filename, const char* content, int size) {
  struct mem_file* file;
  char* buf;
  int i;
  if (!filename || !content || size < 0 || size > 32768) return -1;
  file = find_mem_file(filename);
  if (!file) {
    file = create_mem_file(filename);
    if (!file) return -1;
  }
  if (file->capacity < size + 1) {
    buf = (char*)kmalloc((uint32_t)size + 1u);
    if (!buf) return -1;
    kfree(file->content);
    file->content = buf;
    file->capacity = size + 1;
  }
  for (i = 0; i < size; i++)
    file->content[i] = content[i];
  file->content[size] = '\0';
  file->size = size;
  return 0;
}

typedef void (*puts_fn)(const char*);

static volatile uint16_t* const VGA = (uint16_t*)0xB8000;
static int row = 0, col = 0;

static inline unsigned char inb(unsigned short port) {
  unsigned char data;
  __asm__ volatile ("inb %1, %0" : "=a" (data) : "Nd" (port));
  return data;
}

static inline void outb(unsigned short port, unsigned char data) {
  __asm__ volatile ("outb %0, %1" : : "a" (data), "Nd" (port));
}

unsigned char shell_inb(unsigned short port) { return inb(port); }
void shell_outb(unsigned short port, unsigned char data) { outb(port, data); }

static inline uint16_t inw(unsigned short port) {
  uint16_t data;
  __asm__ volatile ("inw %1, %0" : "=a" (data) : "Nd" (port));
  return data;
}

static inline void outw(unsigned short port, uint16_t data) {
  __asm__ volatile ("outw %0, %1" : : "a" (data), "Nd" (port));
}

static void io_wait_400ns(uint16_t status_port) {
  inb(status_port);
  inb(status_port);
  inb(status_port);
  inb(status_port);
}

static int ata_wait(uint16_t status_port, uint8_t mask, uint8_t value, uint32_t limit, const char* tag, uint32_t drive, uint32_t lba) {
  for (uint32_t t = 0; t < limit; t++) {
    uint8_t st = inb(status_port);
    if (st & 0x01) { trace_event(tag, drive, lba, st); return -1; }     // ERR
    if (st & 0x20) { trace_event(tag, drive, lba, st); return -1; }     // DF
    if ((st & mask) == value) return 0;
  }
  trace_event(tag, drive, lba, inb(status_port));
  return -1;
}

/* Debug ATA: preenchidos em falha (timeout/ERR/DF). Comando dstat imprime. */
uint32_t last_ata_drive;
uint32_t last_ata_lba;
uint8_t last_ata_status;
uint8_t last_ata_error;

static void ata_set_last(uint32_t drive, uint32_t lba, uint16_t status_port, uint16_t base_port) {
  last_ata_drive = drive;
  last_ata_lba = lba;
  last_ata_status = inb(status_port);
  last_ata_error = (last_ata_status & 0x01) ? (uint8_t)inb(base_port + 1) : 0;
}

// Função para ler setor do disco usando ATA PIO
// Executa com interrupts desabilitadas (sem sti). Retorna 0 sucesso; -1/-2/-3 falha.
int disk_read_sector(uint32_t drive, uint32_t lba, void* buffer) {
  __asm__ volatile ("cli");
  TRACE_FUNC("disk_read");
  trace_event("disk_read_args", drive, lba, (uint32_t)buffer);
  if (buffer == 0) return -2;
  if ((uint32_t)buffer < 0x1000u) return -1; /* evita IVT/IDT/BDA */
  uint32_t buf_addr = (uint32_t)buffer;
  if (buf_addr < 0x60000u || buf_addr + 512 > 0x100000u) return -1;
  if (buf_addr >= 0x90000u && buf_addr < 0xA0000u) return -1; /* nunca pilha */
  uint16_t base_port = (drive <= 1) ? 0x1F0 : 0x170;
  uint8_t drive_bit = (drive == 0 || drive == 2) ? 0xE0 : 0xF0;
  uint16_t status_port = base_port + 7;
  uint16_t data_port = base_port;
  outb(base_port + 6, drive_bit | ((lba >> 24) & 0x0F));
  io_wait_400ns(status_port);
  if (ata_wait(status_port, 0x80, 0x00, 1000000, "disk_rd_bsy", drive, lba) < 0) {
    ata_set_last(drive, lba, status_port, base_port);
    return -3;
  }
  outb(base_port + 2, 1);
  outb(base_port + 3, (unsigned char)lba);
  outb(base_port + 4, (unsigned char)(lba >> 8));
  outb(base_port + 5, (unsigned char)(lba >> 16));
  outb(status_port, 0x20);
  if (ata_wait(status_port, 0x88, 0x08, 1000000, "disk_rd_drq", drive, lba) < 0) {
    ata_set_last(drive, lba, status_port, base_port);
    return -3;
  }
  uint16_t* buf = (uint16_t*)buffer;
  for (int i = 0; i < 256; i++) buf[i] = inw(data_port);
  return 0;
}

// Função para escrever setor do disco usando ATA PIO
// Executa com interrupts desabilitadas (sem sti). Retorna 0 sucesso; -1/-2/-3 falha.
int disk_write_sector(uint32_t drive, uint32_t lba, void* buffer) {
  __asm__ volatile ("cli");
  TRACE_FUNC("disk_write");
  trace_event("disk_write_args", drive, lba, (uint32_t)buffer);
  if (buffer == 0) return -2;
  if ((uint32_t)buffer < 0x1000u) return -1;
  uint32_t buf_addr = (uint32_t)buffer;
  if (buf_addr < 0x60000u || buf_addr + 512 > 0x100000u) return -1;
  if (buf_addr >= 0x90000u && buf_addr < 0xA0000u) return -1;
  uint16_t base_port = (drive <= 1) ? 0x1F0 : 0x170;
  uint8_t drive_bit = (drive == 0 || drive == 2) ? 0xE0 : 0xF0;
  uint16_t status_port = base_port + 7;
  uint16_t data_port = base_port;
  outb(base_port + 6, drive_bit | ((lba >> 24) & 0x0F));
  io_wait_400ns(status_port);
  if (ata_wait(status_port, 0x80, 0x00, 1000000, "disk_wr_bsy", drive, lba) < 0) {
    ata_set_last(drive, lba, status_port, base_port);
    return -3;
  }
  outb(base_port + 2, 1);
  outb(base_port + 3, (unsigned char)lba);
  outb(base_port + 4, (unsigned char)(lba >> 8));
  outb(base_port + 5, (unsigned char)(lba >> 16));
  outb(status_port, 0x30);
  if (ata_wait(status_port, 0x88, 0x08, 1000000, "disk_wr_drq", drive, lba) < 0) {
    ata_set_last(drive, lba, status_port, base_port);
    return -3;
  }
  uint16_t* buf = (uint16_t*)buffer;
  for (int i = 0; i < 256; i++) outw(data_port, buf[i]);
  if (ata_wait(status_port, 0x80, 0x00, 1000000, "disk_wr_done", drive, lba) < 0) {
    ata_set_last(drive, lba, status_port, base_port);
    return -3;
  }
  uint8_t final_status = inb(status_port);
  if (final_status & 0x01) {
    ata_set_last(drive, lba, status_port, base_port);
    return -3;
  }
  return 0;
}

void vga_put_at(int r, int c, unsigned char ch) {
  if (r < 0 || r >= 25 || c < 0 || c >= 80) return;
  if (ch < 32) ch = '.';
  VGA[r * 80 + c] = (uint16_t)ch | (uint16_t)(0x07u << 8);
}

void update_cursor(void) {
  int pos = row * 80 + col;
  outb(0x3D4, 0x0F);
  outb(0x3D5, (unsigned char)(pos & 0xFF));
  outb(0x3D4, 0x0E);
  outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
}

/* Posiciona cursor em (r, c) para atualizar área fixa na tela */
void goto_xy(int r, int c) {
  if (r < 0 || r >= 25 || c < 0 || c >= 80) return;
  row = r;
  col = c;
  update_cursor();
}

// Faz scroll da tela para cima (move todas as linhas uma posição para cima)
static void scroll_screen(void) {
  // Move todas as linhas 1-24 para 0-23
  for (int i = 0; i < 24; i++) {
    for (int j = 0; j < 80; j++) {
      VGA[i * 80 + j] = VGA[(i + 1) * 80 + j];
    }
  }
  // Limpa a última linha (linha 24)
  for (int j = 0; j < 80; j++) {
    VGA[24 * 80 + j] = (uint16_t)0x0720; // space
  }
  // Mantém cursor na última linha
  row = 24;
}

static void (*shell_out_fn)(char c);

void shell_set_out(void (*fn)(char c)) { shell_out_fn = fn; }
void (*shell_get_out(void))(char c) { return shell_out_fn; }

static int (*shell_exec_fn)(const char* path, int argc, char** argv);

void shell_set_exec(int (*fn)(const char* path, int argc, char** argv)) { shell_exec_fn = fn; }

void putc(char c) {
  if (shell_out_fn) {
    shell_out_fn(c);
    return;
  }
  if (c == '\n') { 
    row++; 
    col = 0; 
    if (row >= 25) { 
      scroll_screen();
    }
    update_cursor();
    return; 
  }
  if (c == '\r') { 
    col = 0; 
    update_cursor();
    return; 
  }
  if (c == '\b') {
    // Backspace: move cursor para trás
    if (col > 0) {
      col--;
    } else if (row > 0) {
      row--;
      col = 79;
    }
    // Apaga o caractere na posição atual
    VGA[row * 80 + col] = (uint16_t)0x0720; // space
    update_cursor();
    return;
  }
  VGA[row * 80 + col] = (uint16_t)c | (uint16_t)(0x07 << 8);
  col++;
  if (col >= 80) { 
    col = 0; 
    row++; 
    if (row >= 25) {
      scroll_screen();
    }
  }
  update_cursor();
}

void puts_k(const char* s) { while (*s) putc(*s++); }

/* Log para debug de fdisk (partition.c chama) */
void fdisk_log(const char* s) { puts_k(s); }

void clear_line_from_cursor(void) {
  // Limpa da posição atual até o fim da linha
  int start_col = col;
  for (int i = start_col; i < 80; i++) {
    VGA[row * 80 + i] = (uint16_t)0x0720; // space
  }
}

static void erase_chars(int count) {
  // Apaga 'count' caracteres da posição atual, movendo cursor para trás
  for (int i = 0; i < count; i++) {
    if (col > 0) {
      col--;
    } else if (row > 0) {
      row--;
      col = 79;
    }
    VGA[row * 80 + col] = (uint16_t)0x0720; // space
  }
  update_cursor();
}

void clear_screen(void) {
  if (shell_out_fn) {
    shell_out_fn('\f');
    return;
  }
  for (int i = 0; i < 80 * 25; i++) {
    VGA[i] = (uint16_t)0x0720; // space character with gray-on-black attribute
  }
  row = 0;
  col = 0;
  update_cursor();
}

// Estado das teclas modificadoras
static int shift_pressed = 0;

// Scancodes para teclas modificadoras
#define SCANCODE_LSHIFT  0x2A
#define SCANCODE_RSHIFT  0x36

// Partial scancode set 1 -> ascii (sem shift)
static const char scancode_map[128] = {
  0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
  '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a',
  's','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c',
  'v','b','n','m',',','.','/',0,'*',0,' ',
};

// Mapa com Shift pressionado
static const char scancode_map_shift[128] = {
  0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
  '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A',
  'S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C',
  'V','B','N','M','<','>','?',0,'*',0,' ',
};

// Scancodes para setas (extended keys)
#define SCANCODE_UP    0x48
#define SCANCODE_DOWN  0x50
#define SCANCODE_LEFT  0x4B
#define SCANCODE_RIGHT 0x4D
#define SCANCODE_PGUP  0x49
#define SCANCODE_PGDN  0x51
#define SCANCODE_ESC   0x01

static int get_char(void) {
  unsigned char st;
  unsigned char sc;

  /* Polling puro. Sem hlt e sem IRQ0: o timer no vetor 8 (antes do remap)
   * caia no handler de #DF e o iret executava lixo em 0x9FC00 (#UD). */
  for (;;) {
    net_poll();
    st = inb(0x64);
    if (!(st & 1))
      continue;
    sc = inb(0x60);
    if (st & 0x20)
      continue;
    if (sc == 0xFA || sc == 0xFE)
      continue;
    break;
  }

  if (sc == 0xE0) {
    int n = 100000;
    while (!(inb(0x64) & 1) && --n > 0) { }
    if (!(inb(0x64) & 1)) return 0;
    sc = inb(0x60);
    if (sc & 0x80) return 0;
    /* Tabela propria. Um if/switch denso o gcc vira jmp [reg*4]
     * e um buraco dessa tabela executa INT3 (#BP). */
    static const uint16_t ext_keys[256] = {
      [0x48] = KEY_UP,
      [0x50] = KEY_DOWN,
      [0x4B] = KEY_LEFT,
      [0x4D] = KEY_RIGHT,
      [0x49] = KEY_PGUP,
      [0x51] = KEY_PGDN,
      [0x47] = KEY_HOME,
      [0x4F] = KEY_END,
      [0x53] = KEY_DEL,
    };
    return ext_keys[sc];
  }

  if (sc == SCANCODE_LSHIFT || sc == SCANCODE_RSHIFT) {
    shift_pressed = 1;
    return 0;
  }
  if (sc == (SCANCODE_LSHIFT | 0x80) || sc == (SCANCODE_RSHIFT | 0x80)) {
    shift_pressed = 0;
    return 0;
  }
  if (sc & 0x80)
    return 0;

  if (sc < 128) {
    if (shift_pressed)
      return scancode_map_shift[sc];
    return scancode_map[sc];
  }
  return 0;
}

int shell_get_char(void) {
  return get_char();
}

void pause(void) {
  puts_k("Press any key to continue...\n");
  while (get_char() == 0) { }
}

// Histórico de comandos
#define MAX_HISTORY 32
char history[MAX_HISTORY][128];
int history_count = 0;
static int history_index = -1; // -1 significa que não está navegando no histórico

static void add_to_history(const char* cmd) {
  if (!cmd || cmd[0] == '\0') return;
  if (history_count < 0 || history_count > MAX_HISTORY)
    history_count = 0;

  // Não adiciona se for o mesmo comando do último
  if (history_count > 0 && strcmp(history[history_count - 1], cmd) == 0) {
    return;
  }
  
  // Move histórico para frente se estiver cheio
  if (history_count >= MAX_HISTORY) {
    for (int i = 0; i < MAX_HISTORY - 1; i++) {
      int j = 0;
      while (history[i][j] = history[i+1][j]) j++;
    }
    history_count = MAX_HISTORY - 1;
  }
  
  // Adiciona novo comando
  int i = 0;
  while ((history[history_count][i] = cmd[i]) != '\0') i++;
  history_count++;
}

// Carrega histórico do arquivo history.log
static void load_history_from_file(void) {
  const struct file_entry* file = find_file("history.log");
  if (!file) {
    return; // Arquivo não encontrado, não faz nada
  }
  
  // Processa linha por linha
  const char* content = file->content;
  uint32_t size = file->size;
  char line[128];
  int line_pos = 0;
  
  for (uint32_t i = 0; i < size && line_pos < 127; i++) {
    char c = content[i];
    
    if (c == '\n' || c == '\r') {
      // Fim da linha, adiciona ao histórico
      if (line_pos > 0) {
        line[line_pos] = '\0';
        add_to_history(line);
        line_pos = 0;
      }
      // Pula \r\n
      if (c == '\r' && i + 1 < size && content[i + 1] == '\n') {
        i++;
      }
    } else if (c != '\0') {
      // Adiciona caractere à linha
      line[line_pos++] = c;
    }
  }
  
  // Adiciona última linha se não terminou com \n
  if (line_pos > 0) {
    line[line_pos] = '\0';
    add_to_history(line);
  }
}

static unsigned char fold_ch(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
  return c;
}

static int starts_with(const char* name, const char* prefix) {
  int i = 0;
  if (!name || !prefix) return 0;
  while (prefix[i]) {
    if (!name[i]) return 0;
    if (fold_ch((unsigned char)name[i]) != fold_ch((unsigned char)prefix[i]))
      return 0;
    i++;
  }
  return 1;
}

static int add_hit(char hits[][32], int nh, int max, const char* name) {
  int i;
  int k = 0;
  if (!name || !name[0] || nh >= max) return nh;
  for (i = 0; i < nh; i++) {
    if (name_eq(hits[i], name)) return nh;
  }
  while (name[k] && k < 31) {
    hits[nh][k] = name[k];
    k++;
  }
  hits[nh][k] = '\0';
  return nh + 1;
}

/* Completa o ultimo token. Um unico arquivo vira o nome inteiro. */
static void complete_line(char* buf, int* io_i, int max) {
  char names[24][32];
  char hits[16][32];
  char prefix[32];
  int i = *io_i;
  int tok = i;
  int name_at;
  int nnames;
  int nh = 0;
  int k;
  int typed;
  int clen;
  int first;
  char letter;
  while (tok > 0 && buf[tok - 1] != ' ') tok--;
  name_at = tok;
  letter = drives_current();
  if (buf[name_at] && buf[name_at + 1] == ':') {
    letter = buf[name_at];
    name_at += 2;
    if (buf[name_at] == '\\' || buf[name_at] == '/') name_at++;
  }
  typed = i - name_at;
  if (typed < 0) typed = 0;
  if (typed > 31) typed = 31;
  for (k = 0; k < typed; k++) prefix[k] = buf[name_at + k];
  prefix[typed] = '\0';
  first = (tok == 0 && name_at == tok);
  if (first) {
    for (k = 0; k < cmd_count; k++) {
      if (starts_with(commands[k].name, prefix))
        nh = add_hit(hits, nh, 16, commands[k].name);
    }
    for (k = 0; k < executable_count; k++) {
      if (starts_with(executables[k].name, prefix))
        nh = add_hit(hits, nh, 16, executables[k].name);
    }
  }
  nnames = drives_names(letter, names, 24);
  for (k = 0; k < nnames; k++) {
    if (starts_with(names[k], prefix))
      nh = add_hit(hits, nh, 16, names[k]);
  }
  if (nh == 0) return;
  clen = 0;
  while (hits[0][clen]) {
    int same = 1;
    unsigned char a = fold_ch((unsigned char)hits[0][clen]);
    for (k = 1; k < nh; k++) {
      if (fold_ch((unsigned char)hits[k][clen]) != a) {
        same = 0;
        break;
      }
    }
    if (!same) break;
    clen++;
  }
  if (nh > 1 && clen == typed) {
    putc('\n');
    for (k = 0; k < nh; k++) {
      puts_k("  ");
      puts_k(hits[k]);
      puts_k("\n");
    }
    {
      char prompt[64];
      drives_prompt(prompt, (int)sizeof(prompt));
      puts_k(prompt);
    }
    for (k = 0; k < i; k++) putc(buf[k]);
    return;
  }
  if (name_at + clen >= max) clen = max - 1 - name_at;
  if (clen < 0) return;
  while (i > name_at) {
    i--;
    putc('\b');
  }
  for (k = 0; k < clen; k++) {
    buf[name_at + k] = hits[0][k];
    putc(buf[name_at + k]);
  }
  i = name_at + clen;
  buf[i] = '\0';
  *io_i = i;
}

// Move o cursor sem apagar (a linha digitada pode quebrar para a linha de baixo)
static void cursor_back(int n) {
  while (n-- > 0) {
    if (col > 0) {
      col--;
    } else if (row > 0) {
      row--;
      col = 79;
    }
  }
  update_cursor();
}

static void cursor_fwd(int n) {
  while (n-- > 0) {
    if (++col >= 80) {
      col = 0;
      if (row < 24) row++;
    }
  }
  update_cursor();
}

// Reescreve buf[from..len) + 'pad' espaços e volta o cursor para 'from'
static void redraw_tail(const char* buf, int from, int len, int pad) {
  int k;
  for (k = from; k < len; k++) putc(buf[k]);
  for (k = 0; k < pad; k++) putc(' ');
  cursor_back(len - from + pad);
}

// Troca a linha inteira (histórico/ESC); cursor fica no fim
static void replace_line(char* buf, int* len, int* pos, const char* s, int max) {
  int old = *len;
  int n = 0;
  cursor_back(*pos);
  while (s[n] && n < max - 1) {
    buf[n] = s[n];
    n++;
  }
  buf[n] = '\0';
  redraw_tail(buf, 0, n, old > n ? old - n : 0);
  cursor_fwd(n);
  *len = n;
  *pos = n;
}

static void read_line(char* buf, int max) {
  int len = 0;
  int pos = 0;
  int k;
  buf[0] = '\0';
  history_index = -1;
  
  for (;;) {
    int c = get_char();
    if (!c) continue;
    
    if (c == '\r' || c == '\n') {
      cursor_fwd(len - pos);
      putc('\n');
      buf[len] = '\0';
      return;
    }
    
    if (c == KEY_UP || c == KEY_PGUP) {
      // Navega para cima no histórico (comandos mais antigos)
      if (history_count == 0) continue;
      if (history_index == -1) {
        history_index = 0; // Começa no comando mais recente
      } else if (history_index < history_count - 1) {
        history_index++;
      } else {
        continue;
      }
      // Mais antigo = índice maior
      replace_line(buf, &len, &pos, history[history_count - 1 - history_index], max);
      continue;
    }
    
    if (c == KEY_DOWN || c == KEY_PGDN) {
      // Navega para baixo no histórico
      if (history_index == -1) continue;
      history_index--;
      if (history_index >= 0)
        replace_line(buf, &len, &pos, history[history_count - 1 - history_index], max);
      else
        replace_line(buf, &len, &pos, "", max);
      continue;
    }

    if (c == KEY_HOME) {
      cursor_back(pos);
      pos = 0;
      continue;
    }
    if (c == KEY_END) {
      cursor_fwd(len - pos);
      pos = len;
      continue;
    }
    if (c == KEY_LEFT) {
      if (pos > 0) {
        cursor_back(1);
        pos--;
      }
      continue;
    }
    if (c == KEY_RIGHT) {
      if (pos < len) {
        cursor_fwd(1);
        pos++;
      }
      continue;
    }
    
    // Quando digita algo novo, reseta a navegação no histórico
    if (history_index != -1) {
      history_index = -1;
    }
    
    if (c == '\b' || c == 0x7f) {
      // Backspace: remove o caractere antes do cursor
      if (pos > 0) {
        for (k = pos - 1; k < len - 1; k++) buf[k] = buf[k + 1];
        len--;
        pos--;
        buf[len] = '\0';
        cursor_back(1);
        redraw_tail(buf, pos, len, 1);
      }
      continue;
    }

    if (c == KEY_DEL) {
      // Delete: remove o caractere sob o cursor
      if (pos < len) {
        for (k = pos; k < len - 1; k++) buf[k] = buf[k + 1];
        len--;
        buf[len] = '\0';
        redraw_tail(buf, pos, len, 1);
      }
      continue;
    }
    
    if (c == 27) {
      // ESC: limpa toda a linha
      replace_line(buf, &len, &pos, "", max);
      continue;
    }

    if (c == '\t') {
      cursor_fwd(len - pos);
      buf[len] = '\0';
      complete_line(buf, &len, max);
      pos = len;
      continue;
    }

    if (c >= 0x100) continue;
    
    if (len < max - 1) {
      for (k = len; k > pos; k--) buf[k] = buf[k - 1];
      buf[pos] = (char)c;
      len++;
      buf[len] = '\0';
      redraw_tail(buf, pos, len, 0);
      cursor_fwd(1);
      pos++;
    }
  }
}

void shell_read_line(char* buf, int max) {
  read_line(buf, max);
}

// Command table (cmd_entry, cmd_fn, commands, cmd_count, register_builtin em shell.h)
struct cmd_entry commands[48];
int cmd_count = 0;

int register_builtin(const char* name, cmd_fn fn) {
  if (cmd_count < (int)(sizeof(commands)/sizeof(commands[0]))) {
    commands[cmd_count].name = name;
    commands[cmd_count].fn = fn;
    cmd_count++;
    return 0;
  }
  return -1;
}

uint32_t str_to_uint32(const char* str, uint32_t base_param) {
  uint32_t result = 0;
  uint32_t base = base_param;
  if (base == 0) {
    base = 10;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
      base = 16;
      str += 2;
    }
  }

  while (*str) {
    char c = *str;
    uint32_t v;
    if (c >= '0' && c <= '9') {
      v = (uint32_t)(c - '0');
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      v = 10u + (uint32_t)(c - 'a');
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      v = 10u + (uint32_t)(c - 'A');
    } else {
      break;
    }
    result = result * (uint32_t)base + v;
    str++;
  }
  return result;
}

/* int32_to_string e uint32_to_string para cmd_*.c (declarados em shell.h) */
void int32_to_string(int32_t n, char* str, int max_len) {
  uint32_t u;
  int j = 0;
  if (n < 0) {
    if (j < max_len - 1) str[j++] = '-';
    u = (uint32_t)(-(int32_t)n);
  } else {
    u = (uint32_t)n;
  }
  char temp[16];
  int k = 0;
  if (u == 0) temp[k++] = '0';
  else while (u > 0 && k < 15) { temp[k++] = '0' + (u % 10); u /= 10; }
  while (k > 0 && j < max_len - 1) str[j++] = temp[--k];
  str[j] = '\0';
}

void uint32_to_string(uint32_t n, char* str, int max_len) {
  if (n == 0) {
    str[0] = '0';
    str[1] = '\0';
    return;
  }
  char temp[32];
  int k = 0;
  while (n > 0 && k < 31) {
    temp[k++] = '0' + (n % 10);
    n /= 10;
  }
  int j = 0;
  for (int l = k - 1; l >= 0 && j < max_len - 1; l--) {
    str[j++] = temp[l];
  }
  str[j] = '\0';
}

// very simple tokenizer (modest limits)
static int tokenize(char* line, char** argv, int maxargv) {
  int argc = 0;
  char* p = line;
  while (*p && argc < maxargv) {
    while (*p == ' ') ++p;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ') ++p;
    if (*p == ' ') *p++ = '\0';
  }
  return argc;
}

/* "d:" ou "d:\" sozinho troca a unidade corrente. */
static int drive_switch(const char* s, char* letter) {
  char c;
  if (!s || !s[0] || s[1] != ':') return 0;
  c = s[0];
  if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (c < 'A' || c > 'Z') return 0;
  if (s[2] != '\0' && !((s[2] == '\\' || s[2] == '/') && s[3] == '\0'))
    return 0;
  *letter = c;
  return 1;
}

/* Uma linha do prompt, a mesma do shell de texto. line e alterada. */
void shell_exec_line(char* line) {
  char raw[256];
  char* argv[16];
  int argc;
  int i;
  int handled = 0;
  char letter = 0;
  int k = 0;
  if (!line) return;
  while (line[k] && k < (int)sizeof(raw) - 1) {
    raw[k] = line[k];
    k++;
  }
  raw[k] = '\0';
  if (raw[0]) add_to_history(raw);
  argc = tokenize(line, argv, 16);
  if (argc == 0) return;
  if (argc == 1 && drive_switch(argv[0], &letter)) {
    if (drives_set_current(letter) != 0) {
      putc(letter);
      puts_k(": nao montado. Use: mount ");
      putc(letter);
      puts_k(":\n");
    }
    return;
  }
  for (i = 0; i < cmd_count; ++i) {
    if (!stricmp(argv[0], commands[i].name)) {
      commands[i].fn(argc, argv);
      handled = 1;
      break;
    }
  }
  if (!handled) {
    char path[64];
    if (mod_find_exe(argv[0], path, sizeof(path)) == 0) {
      int rc;
      if (shell_exec_fn && shell_exec_fn(path, argc, argv)) return;
      rc = mod_exec(path, argc, argv);
      if (rc <= MOD_ERR_NOTFOUND && rc >= MOD_ERR_DEPTH) {
        puts_k("Erro ao executar: ");
        puts_k(mod_last_error());
        puts_k("\n");
      }
      if (shell_out_fn) {
        /* o console grafico quebra a linha no proprio buffer */
      } else if (col != 0) {
        putc('\n');
      }
    } else {
      puts_k("Bad command or file name\n");
    }
  }
}

extern char __bss_start[];
extern char __bss_end[];

static void bootinfo_panic(const char* msg) {
  volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
  int i;
  /* Nao usa puts_k: row/col vivem na BSS, que ainda nao foi zerada. */
  for (i = 0; msg[i] && msg[i] != '\n' && i < 80; ++i)
    vga[i] = (uint16_t)(unsigned char)msg[i] | (uint16_t)0x0F00;
  serial_init();
  serial_puts(msg);
  for (;;) {
    __asm__ volatile ("cli; hlt");
  }
}

void kmain(const struct BootInfo* bi) {
  __asm__ volatile ("cli");

  if (!bootinfo_valid(bi))
    bootinfo_panic("Erro: BootInfo invalida. Boot abortado.\n");

  /* BSS alem do kernel.bin nao e carregado/zerado pelo boot. */
  {
    char* p = __bss_start;
    while (p < __bss_end)
      *p++ = 0;
  }

  /* So depois de zerar a BSS: g_bootinfo vive nela. */
  bootinfo_set(bi);
  serial_init();
  serial_puts("\n[kernel] BootInfo v1, firmware=");
  serial_puts(bi->firmware == BOOTINFO_FIRMWARE_BIOS ? "BIOS" :
              (bi->firmware == BOOTINFO_FIRMWARE_UEFI ? "UEFI" : "unknown"));
  serial_puts(" kernel=0x");
  serial_hex32((uint32_t)bi->kernel_phys);
  serial_puts(" size=0x");
  serial_hex32((uint32_t)bi->kernel_size);
  serial_puts(" entry=0x");
  serial_hex32((uint32_t)bi->kernel_entry);
  serial_puts(" e820=0x");
  serial_hex32(bi->mem_count);
  serial_puts("\n");

  mm_init();
  serial_puts("\n[kernel] boot 64-bit, serial log at COM1 (use qemu -serial file:...)\n");

  pic_mask_all();
  pic_remap(0x20, 0x28);         /* IRQ0..7 -> 0x20; deixa o PIC mascarado */
  idt_init();
  /* Sem sti. O shell e o WIN leem o PS/2 por polling. */

  vga_capture_bios_font();
  vga_force_text_mode();         /* a leitura da fonte mexe nos registros VGA */
  clear_screen();
  puts_k("SO DOS 1.0\n");
  puts_k("64-bit kernel. Type HELP or WIN.\n\n");
  net_init();
  httpd_start(8797);
  update_cursor();

  register_builtin("help", cmd_help);
  register_builtin("echo", cmd_echo);
  register_builtin("ls", cmd_ls);
  register_builtin("clear", cmd_clear);
  register_builtin("cls", cmd_cls);
  register_builtin("history", cmd_history);
  register_builtin("type", cmd_type);
  register_builtin("pause", cmd_pause);
  register_builtin("load", cmd_load);
  register_builtin("edit", cmd_edit);
  register_builtin("asm", cmd_asm);
  register_builtin("run", cmd_run);
  register_builtin("mount", cmd_mount);
  register_builtin("dir", cmd_dir);
  register_builtin("cd", cmd_cd);
  register_builtin("md", cmd_md);
  register_builtin("rd", cmd_rd);
  register_builtin("cat", cmd_cat);
  register_builtin("fdisk", cmd_fdisk);
  register_builtin("format", cmd_format);
  register_builtin("copy", cmd_copy);
  register_builtin("disks", cmd_disks);
  register_builtin("free", cmd_free);
  register_builtin("df", cmd_df);
  register_builtin("dstat", cmd_dstat);
  register_builtin("ata", cmd_dstat);
  register_builtin("visual", cmd_visual);
  register_builtin("mouse", cmd_mouse);
  register_builtin("win", cmd_win);
  register_builtin("win.exe", cmd_win);
  register_builtin("ping", cmd_ping);
  register_builtin("dlls", cmd_dlls);
  register_builtin("shutdown", cmd_shutdown);

  drives_init();
  drives_automount();
  net_demo();
  {
    char path[64];
    if (mod_find_exe("HTTPD.EXE", path, (int)sizeof(path)) == 0)
      mod_exec(path, 0, 0);
  }

  puts_k("OS Ready.\n");
  /* Unico trafego serial depois do boot ate ao visual: shell nao usa COM1 (só VGA). */
  serial_puts("[kernel] shell ready — mais linhas na serial apos o comando 'visual'\n");

  char line[256];

  for (;;) {
    {
      char prompt[64];
      drives_prompt(prompt, (int)sizeof(prompt));
      puts_k(prompt);
    }
    read_line(line, sizeof(line));
    shell_exec_line(line);
  }
}

// Minimal strcmp for freestanding
int strcmp(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *(unsigned char*)a - *(unsigned char*)b;
}

static int stricmp(const char* a, const char* b) {
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
    if (ca != cb) return (int)ca - (int)cb;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
