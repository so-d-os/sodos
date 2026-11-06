/* time.c - time.h sobre o RTC (k_date + k_time, UTC) e o k_ms.
 * Sem tabela de fusos: localtime e igual a gmtime. */
#include <time.h>
#include <locale.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <so.h>

int daylight = 0;
long timezone = 0;
char* tzname[2] = {(char*)"UTC", (char*)"UTC"};

void tzset(void) {}

/* dias desde 1970-01-01 (algoritmo civil de H. Hinnant) */
static long days_from_civil(long y, int m, int d) {
  long era, yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

time_t time(time_t* t) {
  int d = k_date(), hms = k_time();
  time_t r;
  if (d <= 0) r = 0;
  else {
    r = (time_t)days_from_civil(d / 10000, d / 100 % 100, d % 100) * 86400;
    if (hms > 0) r += (hms / 10000) * 3600 + (hms / 100 % 100) * 60 + hms % 100;
  }
  if (t) *t = r;
  return r;
}

clock_t clock(void) { return (clock_t)k_ms(); }

#if !(defined(__GNUC__) && defined(__x86_64__) && !defined(__SSE2__))
double difftime(time_t a, time_t b) { return (double)a - (double)b; }
#endif

struct tm* gmtime_r(const time_t* tp, struct tm* tm) {
  long t = (long)*tp;
  long days = t / 86400, secs = t % 86400;
  long z, era, doe, yoe, y, doy, mp;
  if (secs < 0) {
    secs += 86400;
    days--;
  }
  tm->tm_hour = (int)(secs / 3600);
  tm->tm_min = (int)(secs / 60 % 60);
  tm->tm_sec = (int)(secs % 60);
  tm->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 foi quinta */
  z = days + 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = z - era * 146097;
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  tm->tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
  tm->tm_mon = (int)(mp < 10 ? mp + 2 : mp - 10);
  if (tm->tm_mon <= 1) y++;
  tm->tm_year = (int)(y - 1900);
  tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
  tm->tm_isdst = 0;
#if __SIZEOF_POINTER__ == 8
  tm->tm_gmtoff = 0;
  tm->tm_zone = "UTC";
#endif
  return tm;
}

struct tm* localtime_r(const time_t* t, struct tm* tm) { return gmtime_r(t, tm); }

static struct tm g_tm;
struct tm* gmtime(const time_t* t) { return gmtime_r(t, &g_tm); }
struct tm* localtime(const time_t* t) { return gmtime_r(t, &g_tm); }

/* normaliza os campos (mes 13, dia 0, segundos negativos...) e devolve time_t */
time_t timegm(struct tm* tm) {
  long y = tm->tm_year + 1900L, mon = tm->tm_mon;
  long t;
  y += mon / 12;
  mon %= 12;
  if (mon < 0) {
    mon += 12;
    y--;
  }
  t = days_from_civil(y, (int)mon + 1, 1) + tm->tm_mday - 1;
  t = t * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
  gmtime_r(&t, tm);
  return t;
}

time_t mktime(struct tm* tm) { return timegm(tm); }

static const char* wday_name[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char* wday_full[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                   "Friday", "Saturday" };
static const char* mon_name[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char* mon_full[] = { "January", "February", "March", "April", "May", "June", "July",
                                  "August", "September", "October", "November", "December" };

char* asctime_r(const struct tm* tm, char* buf) {
  sprintf(buf, "%.3s %.3s%3d %02d:%02d:%02d %d\n", wday_name[(tm->tm_wday % 7 + 7) % 7],
          mon_name[(tm->tm_mon % 12 + 12) % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
          1900 + tm->tm_year);
  return buf;
}

static char g_asc[32];
char* asctime(const struct tm* tm) { return asctime_r(tm, g_asc); }

char* ctime_r(const time_t* t, char* buf) {
  struct tm tm;
  return asctime_r(localtime_r(t, &tm), buf);
}

char* ctime(const time_t* t) { return ctime_r(t, g_asc); }

struct out {
  char* s;
  size_t max;
  size_t n;
};

static void put_s(struct out* o, const char* s) {
  while (*s) {
    if (o->n + 1 < o->max) o->s[o->n] = *s;
    o->n++;
    s++;
  }
}

static void put_num(struct out* o, long v, int width, char pad) {
  char b[24];
  char f[8];
  f[0] = '%';
  if (pad == '0') {
    f[1] = '0';
    f[2] = (char)('0' + width);
    f[3] = 'l';
    f[4] = 'd';
    f[5] = 0;
  } else {
    f[1] = (char)('0' + width);
    f[2] = 'l';
    f[3] = 'd';
    f[4] = 0;
  }
  sprintf(b, f, v);
  put_s(o, b);
}

/* numero da semana ISO 8601 e o ano a que ela pertence */
static int iso_week(const struct tm* tm, long* year) {
  long y = tm->tm_year + 1900L;
  int wday = (tm->tm_wday + 6) % 7; /* segunda = 0 */
  int wk = (tm->tm_yday - wday + 10) / 7;
  if (wk < 1) {
    long py = y - 1;
    int pdays = 365 + is_leap(py);
    int pyday = tm->tm_yday + pdays;
    wk = (pyday - wday + 10) / 7;
    y = py;
  } else if (wk > 52) {
    int days = 365 + is_leap(y);
    if (tm->tm_yday - wday + 3 >= days) {
      wk = 1;
      y++;
    }
  }
  *year = y;
  return wk;
}

static void fmt_tm(struct out* o, const char* f, const struct tm* tm) {
  for (; *f; f++) {
    char c;
    if (*f != '%') {
      char one[2];
      one[0] = *f;
      one[1] = 0;
      put_s(o, one);
      continue;
    }
    f++;
    if (*f == 'E' || *f == 'O') f++;
    c = *f;
    if (!c) break;
    switch (c) {
      case 'a': put_s(o, wday_name[(tm->tm_wday % 7 + 7) % 7]); break;
      case 'A': put_s(o, wday_full[(tm->tm_wday % 7 + 7) % 7]); break;
      case 'b': case 'h': put_s(o, mon_name[(tm->tm_mon % 12 + 12) % 12]); break;
      case 'B': put_s(o, mon_full[(tm->tm_mon % 12 + 12) % 12]); break;
      case 'c': fmt_tm(o, "%a %b %e %H:%M:%S %Y", tm); break;
      case 'C': put_num(o, (tm->tm_year + 1900L) / 100, 2, '0'); break;
      case 'd': put_num(o, tm->tm_mday, 2, '0'); break;
      case 'D': fmt_tm(o, "%m/%d/%y", tm); break;
      case 'e': put_num(o, tm->tm_mday, 2, ' '); break;
      case 'F': fmt_tm(o, "%Y-%m-%d", tm); break;
      case 'g': case 'G': case 'V': {
        long y;
        int w = iso_week(tm, &y);
        if (c == 'V') put_num(o, w, 2, '0');
        else if (c == 'g') put_num(o, y % 100, 2, '0');
        else put_num(o, y, 1, '0');
        break;
      }
      case 'H': put_num(o, tm->tm_hour, 2, '0'); break;
      case 'I': put_num(o, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, 2, '0'); break;
      case 'j': put_num(o, tm->tm_yday + 1, 3, '0'); break;
      case 'm': put_num(o, tm->tm_mon + 1, 2, '0'); break;
      case 'M': put_num(o, tm->tm_min, 2, '0'); break;
      case 'n': put_s(o, "\n"); break;
      case 'p': put_s(o, tm->tm_hour < 12 ? "AM" : "PM"); break;
      case 'r': fmt_tm(o, "%I:%M:%S %p", tm); break;
      case 'R': fmt_tm(o, "%H:%M", tm); break;
      case 's': {
        struct tm t = *tm;
        put_num(o, (long)timegm(&t), 1, '0');
        break;
      }
      case 'S': put_num(o, tm->tm_sec, 2, '0'); break;
      case 't': put_s(o, "\t"); break;
      case 'T': fmt_tm(o, "%H:%M:%S", tm); break;
      case 'u': put_num(o, tm->tm_wday ? tm->tm_wday : 7, 1, '0'); break;
      case 'U': put_num(o, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2, '0'); break;
      case 'w': put_num(o, tm->tm_wday, 1, '0'); break;
      case 'W': put_num(o, (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7, 2, '0'); break;
      case 'x': fmt_tm(o, "%m/%d/%y", tm); break;
      case 'X': fmt_tm(o, "%H:%M:%S", tm); break;
      case 'y': put_num(o, (tm->tm_year + 1900L) % 100, 2, '0'); break;
      case 'Y': put_num(o, tm->tm_year + 1900L, 1, '0'); break;
      case 'z': put_s(o, "+0000"); break;
      case 'Z': put_s(o, "UTC"); break;
      case '%': put_s(o, "%"); break;
      default: {
        char two[3];
        two[0] = '%';
        two[1] = c;
        two[2] = 0;
        put_s(o, two);
      }
    }
  }
}

size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm) {
  struct out o;
  o.s = s;
  o.max = max;
  o.n = 0;
  fmt_tm(&o, fmt, tm);
  if (o.n >= max) {
    if (max) s[0] = 0;
    return 0;
  }
  s[o.n] = 0;
  return o.n;
}

/* REALTIME: segundos do RTC + ms do relogio de maquina (o RTC so tem segundos) */
#if __SIZEOF_POINTER__ == 8
extern int sched_yield(void) __attribute__((weak));
#endif

int clock_gettime(clockid_t id, struct timespec* ts) {
  unsigned ms = k_ms();
#if __SIZEOF_POINTER__ == 8
  /* Threads cooperativas: quem espera olhando o relogio tambem precisa ceder. */
  static unsigned calls;
  if (sched_yield && (++calls & 63u) == 0) sched_yield();
#endif
  if (id == CLOCK_MONOTONIC) {
    ts->tv_sec = (time_t)(ms / 1000);
    ts->tv_nsec = (long)(ms % 1000) * 1000000L;
    return 0;
  }
  ts->tv_sec = time(0);
  ts->tv_nsec = (long)(ms % 1000) * 1000000L;
  return 0;
}

int gettimeofday(struct timeval* tv, void* tz) {
  struct timespec ts;
  (void)tz;
  clock_gettime(CLOCK_REALTIME, &ts);
  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = ts.tv_nsec / 1000;
  return 0;
}

/* C11. O SODOS so tem o relogio de parede (UTC). */
int timespec_get(struct timespec* ts, int base) {
  if (!ts || base != TIME_UTC) return 0;
  clock_gettime(CLOCK_REALTIME, ts);
  return base;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
  unsigned start = k_ms();
  unsigned want = (unsigned)req->tv_sec * 1000u + (unsigned)(req->tv_nsec / 1000000L);
  while (k_ms() - start < want) {}
  if (rem) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

size_t strftime_l(char* s, size_t max, const char* fmt, const struct tm* tm, locale_t loc) { (void)loc; return strftime(s, max, fmt, tm); }
int strcoll_l(const char* a, const char* b, locale_t loc) { (void)loc; return strcmp(a,b); }
size_t strxfrm_l(char* d, const char* s, size_t n, locale_t loc) { (void)loc; size_t l = strlen(s); if (n) { size_t c = l < n-1 ? l : n-1; memcpy(d,s,c); d[c]=0; } return l; }
