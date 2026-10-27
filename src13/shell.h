/* shell.h - API do shell para módulos de comando (cmd_*.c) */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int (*cmd_fn)(int argc, char** argv);

struct cmd_entry {
  const char* name;
  cmd_fn fn;
};

extern struct cmd_entry commands[48];
extern int cmd_count;
int register_builtin(const char* name, cmd_fn fn);

void puts_k(const char* s);
void putc(char c);
/* Enquanto fn nao e nulo, putc e clear_screen mandam o texto para fn.
 * clear_screen manda '\\f'. */
void shell_set_out(void (*fn)(char c));
void (*shell_get_out(void))(char c);
/* Enquanto fn nao e nulo, o .EXE achado por shell_exec_line vai para fn.
 * fn devolve 0 quando nao quer o programa, e ai roda o mod_exec de sempre. */
void shell_set_exec(int (*fn)(const char* path, int argc, char** argv));
/* Executa uma linha como o prompt do boot. line e modificada. */
void shell_exec_line(char* line);
void vga_put_at(int r, int c, unsigned char ch);
void clear_screen(void);

static inline int name_eq(const char* a, const char* b) {
  int i = 0;
  if (!a || !b) return 0;
  while (a[i] && b[i]) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
    i++;
  }
  return a[i] == '\0' && b[i] == '\0';
}
void update_cursor(void);
void goto_xy(int r, int c);
void clear_line_from_cursor(void);
void pause(void);
int shell_get_char(void);

#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103
#define KEY_PGUP  0x104
#define KEY_PGDN  0x105
#define KEY_HOME  0x106
#define KEY_END   0x107
#define KEY_DEL   0x108

int strcmp(const char* a, const char* b);
uint32_t str_to_uint32(const char* str, uint32_t base);
void uint32_to_string(uint32_t n, char* str, int max_len);
void int32_to_string(int32_t n, char* str, int max_len);

struct mem_file {
  char name[32];
  char* content;
  int size;
  int capacity;
};
extern struct mem_file mem_files[16];
extern int mem_file_count;
struct mem_file* find_mem_file(const char* name);
int write_to_mem_file(const char* filename, const char* content);
int write_mem_bytes(const char* filename, const char* content, int size);

struct file_entry {
  const char* name;
  const char* content;
  uint32_t size;
};
extern const struct file_entry* find_file(const char* name);
extern const struct file_entry files[];
extern const int file_count;

struct executable_entry {
  const char* name;
  const uint8_t* data;
  uint32_t size;
  uint32_t entry_point;
};
extern const struct executable_entry* find_executable(const char* name);
extern const struct executable_entry executables[];
extern const int executable_count;

extern char history[32][128];
extern int history_count;

unsigned char shell_inb(unsigned short port);
void shell_outb(unsigned short port, unsigned char data);

#define FDISK_LIST_OUT_BUF  0x00080200u
#define FDISK_DRIVE_STR_BUF 0x00080600u

void fdisk_log(const char* s);

/* Comandos (definidos em cmd_*.c) */
int cmd_help(int argc, char** argv);
int cmd_echo(int argc, char** argv);
int cmd_ls(int argc, char** argv);
int cmd_clear(int argc, char** argv);
int cmd_cls(int argc, char** argv);
int cmd_history(int argc, char** argv);
int cmd_type(int argc, char** argv);
int cmd_pause(int argc, char** argv);
int cmd_load(int argc, char** argv);
int cmd_edit(int argc, char** argv);
int cmd_asm(int argc, char** argv);
int asm_build(const char* path);
int cmd_run(int argc, char** argv);
int cmd_mount(int argc, char** argv);
int cmd_dir(int argc, char** argv);
int cmd_cd(int argc, char** argv);
int cmd_md(int argc, char** argv);
int cmd_rd(int argc, char** argv);
int cmd_cat(int argc, char** argv);
int cmd_fdisk(int argc, char** argv);
int cmd_format(int argc, char** argv);
int cmd_copy(int argc, char** argv);
int cmd_disks(int argc, char** argv);
int cmd_free(int argc, char** argv);
int cmd_df(int argc, char** argv);
int cmd_dstat(int argc, char** argv);
int cmd_visual(int argc, char** argv);
int cmd_mouse(int argc, char** argv);
int cmd_win(int argc, char** argv);
int cmd_ping(int argc, char** argv);
int cmd_dlls(int argc, char** argv);
int cmd_shutdown(int argc, char** argv);
