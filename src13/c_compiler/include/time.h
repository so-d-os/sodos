#ifndef _TIME_H
#define _TIME_H

/* Hora do RTC (k_date/k_time). Sem fuso: localtime == gmtime. */
#include <stddef.h>
#include <sys/types.h>

#define CLOCKS_PER_SEC 1000
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define TIME_UTC 1
typedef int clockid_t;

struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;    /* 0..11 */
  int tm_year;   /* ano - 1900 */
  int tm_wday;   /* 0 = domingo */
  int tm_yday;
  int tm_isdst;
};

struct timespec {
  time_t tv_sec;
  long tv_nsec;
};

time_t time(time_t* t);
clock_t clock(void);
double difftime(time_t a, time_t b);
time_t mktime(struct tm* tm);
time_t timegm(struct tm* tm);
struct tm* gmtime(const time_t* t);
struct tm* localtime(const time_t* t);
struct tm* gmtime_r(const time_t* t, struct tm* out);
struct tm* localtime_r(const time_t* t, struct tm* out);
char* asctime(const struct tm* tm);
char* ctime(const time_t* t);
char* asctime_r(const struct tm* tm, char* buf);
char* ctime_r(const time_t* t, char* buf);
size_t strftime(char* s, size_t max, const char* fmt, const struct tm* tm);
int clock_gettime(clockid_t id, struct timespec* ts);
int nanosleep(const struct timespec* req, struct timespec* rem);
void tzset(void);
extern int daylight;
extern long timezone;

#endif
