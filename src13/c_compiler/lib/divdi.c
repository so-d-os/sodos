/* divdi.c - Divisao de 64 bits que o i386 nao tem (o cc e o gcc chamam estas). */

unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d,
                                unsigned long long* rem) {
  unsigned long long q = 0;
  unsigned long long r = 0;
  int i;
  if (d == 0) {
    if (rem) *rem = 0;
    return 0;
  }
  if ((n >> 32) == 0 && (d >> 32) == 0) {
    unsigned a = (unsigned)n;
    unsigned b = (unsigned)d;
    if (rem) *rem = a % b;
    return a / b;
  }
  for (i = 63; i >= 0; i--) {
    r = (r << 1) | ((n >> i) & 1);
    if (r >= d) {
      r -= d;
      q |= 1ULL << i;
    }
  }
  if (rem) *rem = r;
  return q;
}

unsigned long long __udivdi3(unsigned long long n, unsigned long long d) {
  return __udivmoddi4(n, d, 0);
}

unsigned long long __umoddi3(unsigned long long n, unsigned long long d) {
  unsigned long long r;
  __udivmoddi4(n, d, &r);
  return r;
}

long long __divdi3(long long n, long long d) {
  int neg = 0;
  unsigned long long q;
  if (n < 0) {
    n = -n;
    neg = !neg;
  }
  if (d < 0) {
    d = -d;
    neg = !neg;
  }
  q = __udivmoddi4((unsigned long long)n, (unsigned long long)d, 0);
  return neg ? -(long long)q : (long long)q;
}

long long __moddi3(long long n, long long d) {
  int neg = 0;
  unsigned long long r;
  if (n < 0) {
    n = -n;
    neg = 1;
  }
  if (d < 0) d = -d;
  __udivmoddi4((unsigned long long)n, (unsigned long long)d, &r);
  return neg ? -(long long)r : (long long)r;
}
