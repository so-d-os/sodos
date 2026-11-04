#include <pthread.h>
#include <stdio.h>

int main(void) {
  pthread_mutex_t m;
  printf("MUT64: init\n");
  if (pthread_mutex_init(&m, 0) != 0) return 2;
  printf("MUT64: lock\n");
  pthread_mutex_lock(&m);
  pthread_mutex_unlock(&m);
  printf("MUT64: destroy\n");
  pthread_mutex_destroy(&m);
  printf("MUT64: ok\n");
  return 0;
}
