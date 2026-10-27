#include "shell.h"

int cmd_history(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (history_count == 0) {
    puts_k("No history.\n");
    return 0;
  }
  for (int i = 0; i < history_count; i++) {
    char num_str[16];
    int n = i + 1;
    int j = 0;
    if (n == 0) {
      num_str[j++] = '0';
    } else {
      char temp[16];
      int k = 0;
      while (n > 0) {
        temp[k++] = '0' + (n % 10);
        n /= 10;
      }
      for (int l = k - 1; l >= 0; l--)
        num_str[j++] = temp[l];
    }
    num_str[j] = '\0';
    puts_k(num_str);
    puts_k(": ");
    puts_k(history[i]);
    puts_k("\n");
  }
  return 0;
}
