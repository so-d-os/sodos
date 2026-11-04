#include <prinit.h>
#include <prlock.h>
#include <prcvar.h>
#include <prtime.h>
#include <stdio.h>

extern void k_dbg(const char* s, int n);

static void dbg_hex(unsigned long long v) {
  char b[16];
  int i;
  for (i = 15; i >= 0; i--) {
    unsigned d = (unsigned)(v & 15);
    b[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v >>= 4;
  }
  k_dbg(b, 16);
  k_dbg("\n", 1);
}

int main(void) {
  k_dbg("T0\n", 3);
  printf("MIN64: antes do PR_Init\n");
  k_dbg("T1\n", 3);
  PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 0);
  k_dbg("T2\n", 3);
  k_dbg("N0\n", 3);
  PRTime t = PR_Now();
  k_dbg("N1\n", 3);
  printf("MIN64: PR_Init ok (t=%d)\n", (int)(t & 0x7fffffff));
  k_dbg("L0\n", 3);
  PRLock* l = PR_NewLock();
  k_dbg("L1\n", 3);
  PR_Lock(l);
  k_dbg("L2\n", 3);
  PR_Unlock(l);
  k_dbg("L3\n", 3);
  k_dbg("V0\n", 3);
  const char* v = PR_GetVersion();
  k_dbg("V1\n", 3);
  printf("MIN64: versao %s\n", v);
  k_dbg("W0\n", 3);
  PRCondVar* cv = PR_NewCondVar(l);
  k_dbg("W1\n", 3);
  PR_Lock(l);
  k_dbg("W2\n", 3);
  (void)PR_WaitCondVar(cv, PR_MillisecondsToInterval(10));
  k_dbg("W3\n", 3);
  PR_Unlock(l);
  k_dbg("W4\n", 3);
  k_dbg("S0\n", 3);
  PR_Sleep(PR_MillisecondsToInterval(10));
  k_dbg("S1\n", 3);
  return 0;
}
