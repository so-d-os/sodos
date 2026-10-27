// editor.c - Editor de texto simples

// Estrutura para arquivo em memória
struct mem_file {
  char name[32];
  char* content;
  int size;
  int capacity;
};

static struct mem_file files[16];
static int file_count = 0;

// Função auxiliar para putc (usa puts com string de 1 char)
static void editor_putc(void (*puts)(const char*), char c) {
  char s[2] = {c, '\0'};
  puts(s);
}

// Busca arquivo por nome
static struct mem_file* find_file(const char* name) {
  for (int i = 0; i < file_count; i++) {
    int j = 0;
    while (files[i].name[j] == name[j] && files[i].name[j] != '\0') j++;
    if (files[i].name[j] == '\0' && name[j] == '\0') {
      return &files[i];
    }
  }
  return 0;
}

// Cria novo arquivo
static struct mem_file* create_file(const char* name) {
  if (file_count >= 16) return 0;
  
  struct mem_file* f = &files[file_count++];
  int i = 0;
  while (name[i] != '\0' && i < 31) {
    f->name[i] = name[i];
    i++;
  }
  f->name[i] = '\0';
  
  f->capacity = 1024;
  f->content = (char*)0x40000; // Endereço fixo para memória do editor
  f->size = 0;
  f->content[0] = '\0';
  
  return f;
}

// Editor principal - assinatura simplificada para compatibilidade
// Função auxiliar para obter caractere
static inline unsigned char editor_inb(unsigned short port) {
  unsigned char data;
  __asm__ volatile ("inb %1, %0" : "=a" (data) : "Nd" (port));
  return data;
}

// Estado do Shift no editor
static int editor_shift_pressed = 0;

// Scancodes para teclas modificadoras
#define EDITOR_SCANCODE_LSHIFT  0x2A
#define EDITOR_SCANCODE_RSHIFT  0x36

// Mapa de scancodes básico (sem shift)
static const char editor_scancode_map[128] = {
  0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
  '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a',
  's','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c',
  'v','b','n','m',',','.','/',0,'*',0,' ',
};

// Mapa com Shift pressionado
static const char editor_scancode_map_shift[128] = {
  0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
  '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A',
  'S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C',
  'V','B','N','M','<','>','?',0,'*',0,' ',
};

static int editor_getc(void) {
  // Espera até haver dados disponíveis
  int timeout = 0;
  while (!(editor_inb(0x64) & 1)) {
    timeout++;
    if (timeout > 1000000) return 0; // Timeout
    // Pequeno delay
    for (volatile int d = 0; d < 100; d++);
  }
  
  unsigned char sc = editor_inb(0x60);
  
  // Detecta teclas modificadoras (Shift)
  if (sc == EDITOR_SCANCODE_LSHIFT || sc == EDITOR_SCANCODE_RSHIFT) {
    editor_shift_pressed = 1;
    return 0;
  }
  if (sc == (EDITOR_SCANCODE_LSHIFT | 0x80) || sc == (EDITOR_SCANCODE_RSHIFT | 0x80)) {
    editor_shift_pressed = 0;
    return 0;
  }
  
  // Ignora key releases de outras teclas
  if (sc & 0x80) {
    return 0;
  }
  
  // Extended keys (0xE0 prefix)
  if (sc == 0xE0) {
    timeout = 0;
    while (!(editor_inb(0x64) & 1)) {
      timeout++;
      if (timeout > 1000) return 0;
      for (volatile int d = 0; d < 10; d++);
    }
    sc = editor_inb(0x60);
    if (sc & 0x80) return 0;
    // Ignora teclas extended por enquanto (setas, etc)
    return 0;
  }
  
  // Mapeia scancode para caractere (com ou sem Shift)
  if (sc < 128) {
    if (editor_shift_pressed) {
      int result = editor_scancode_map_shift[sc];
      if (result != 0) return result;
    } else {
      int result = editor_scancode_map[sc];
      if (result != 0) return result;
    }
  }
  return 0;
}

// Lê o nome do arquivo de um endereço fixo de memória (0x50000)
// onde o kernel armazena o nome do arquivo antes de chamar o editor
static const char* get_filename(void) {
  return (const char*)0x50000;
}

int editor_main(void (*puts)(const char*)) {
  const char* filename = get_filename();
  
  if (!filename || filename[0] == '\0') {
    puts("Error: No filename provided\n");
    return 1;
  }
  struct mem_file* file = find_file(filename);
  
  if (!file) {
    // Cria novo arquivo
    file = create_file(filename);
    if (!file) {
      puts("Error: Cannot create file (max files reached)\n");
      return 1;
    }
    puts("New file created: ");
    puts(filename);
    puts("\n");
  } else {
    puts("Editing: ");
    puts(filename);
    puts("\n");
  }
  
  // Modo de edição
  int cursor_pos = 0;
  int line_start = 0;
  char line[256];
  int line_pos = 0;
  
  puts("=== Text Editor ===\n");
  puts("Commands:\n");
  puts("  Type text to edit\n");
  puts("  BACKSPACE: Delete character\n");
  puts("  ESC: Save and exit\n");
  puts("  ENTER: New line\n");
  puts("===================\n\n");
  
  // Mostra conteúdo atual
  if (file->size > 0) {
    puts("Current content:\n");
    for (int i = 0; i < file->size; i++) {
      editor_putc(puts, file->content[i]);
    }
    puts("\n--- End of file ---\n");
    puts("Start editing (ESC to save and exit):\n");
  } else {
    puts("Empty file. Start typing (ESC to save and exit):\n");
  }
  
  // Loop de edição
  puts(">>> ");
  for (;;) {
    int c = editor_getc();
    
    if (c == 0) {
      // Pequeno delay para não consumir CPU
      for (volatile int d = 0; d < 1000; d++);
      continue;
    }
    
    // ESC - salvar e sair
    if (c == 27) {
      // Salva o conteúdo da linha atual antes de sair
      if (line_pos > 0) {
        if (file->size + line_pos < file->capacity - 1) {
          for (int i = 0; i < line_pos; i++) {
            file->content[file->size++] = line[i];
          }
          file->content[file->size] = '\0';
        }
        line_pos = 0;
      }
      puts("\nSaving file...\n");
      break;
    }
    
    if (c == '\r' || c == '\n') {
      // Nova linha
      if (line_pos < 255) {
        line[line_pos++] = '\n';
        line[line_pos] = '\0';
        
        // Adiciona à memória do arquivo imediatamente
        if (file->size + line_pos < file->capacity - 1) {
          for (int i = 0; i < line_pos; i++) {
            file->content[file->size++] = line[i];
          }
          file->content[file->size] = '\0';
        }
        
        editor_putc(puts, '\n');
        line_pos = 0;
      }
      continue;
    }
    
    if (c == '\b' || c == 0x7f) {
      // Backspace
      if (line_pos > 0) {
        line_pos--;
        editor_putc(puts, '\b');
        editor_putc(puts, ' ');
        editor_putc(puts, '\b');
      } else if (file->size > 0) {
        // Remove do arquivo
        file->size--;
        file->content[file->size] = '\0';
        editor_putc(puts, '\b');
        editor_putc(puts, ' ');
        editor_putc(puts, '\b');
      }
      continue;
    }
    
    // Caractere normal (inclui letras, números, símbolos)
    if (line_pos < 255 && c != 0 && c != 27) {
      // Aceita qualquer caractere válido (incluindo espaço, que é 32)
      if (c >= 32 || c == '\n' || c == '\t') {
        line[line_pos++] = (char)c;
        line[line_pos] = '\0';
        editor_putc(puts, (char)c);
      }
    }
  }
  
  // Garante que todo o conteúdo foi salvo
  // (já foi salvo no ESC, mas deixamos aqui como backup)
  if (line_pos > 0) {
    if (file->size + line_pos < file->capacity - 1) {
      for (int i = 0; i < line_pos; i++) {
        file->content[file->size++] = line[i];
      }
      file->content[file->size] = '\0';
    }
  }
  
  // Confirma que o arquivo foi salvo
  puts("File saved: ");
  puts(filename);
  puts("\n");
  
  // Mostra informações do arquivo salvo
  puts("Size: ");
  char size_str[16];
  int n = file->size;
  int j = 0;
  if (n == 0) {
    size_str[j++] = '0';
  } else {
    char temp[16];
    int k = 0;
    while (n > 0) {
      temp[k++] = '0' + (n % 10);
      n /= 10;
    }
    for (int l = k - 1; l >= 0; l--) {
      size_str[j++] = temp[l];
    }
  }
  size_str[j] = '\0';
  puts(size_str);
  puts(" bytes\n");
  
  // Mostra preview do conteúdo salvo
  if (file->size > 0) {
    puts("Content preview (first 100 chars):\n");
    int preview_len = file->size > 100 ? 100 : file->size;
    for (int i = 0; i < preview_len; i++) {
      editor_putc(puts, file->content[i]);
    }
    if (file->size > 100) {
      puts("...\n");
    } else {
      puts("\n");
    }
  }
  
  return 0;
}
