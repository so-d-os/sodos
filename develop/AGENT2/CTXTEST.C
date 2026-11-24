#include "stdio.h"
#include "string.h"
#include "so.h"
int main(void) {
char pwd[128];
char lst[1024];
unsigned tot = 0;
unsigned fre = 0;
int i;
int cnt;
struct k_hwstat hw;
