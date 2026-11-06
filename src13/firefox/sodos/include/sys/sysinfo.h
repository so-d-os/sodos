/* sys/sysinfo.h (SODOS) - declaracoes. */
#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H
#ifdef __cplusplus
extern "C" {
#endif
struct sysinfo {
  long uptime;
  unsigned long loads[3];
  unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  unsigned short procs;
  unsigned long totalhigh, freehigh;
  unsigned int mem_unit;
};
int sysinfo(struct sysinfo* info);
int get_nprocs(void);
int get_nprocs_conf(void);
#ifdef __cplusplus
}
#endif
#endif
