/* thermal.h - temperatura e indicadores do PC (kernel/thermal.c).
 * struct k_hwstat tem o mesmo layout em c_compiler/include/so.h (k_hwstat). */
#pragma once
#include <stdint.h>

#define HW_HIST 64          /* amostras guardadas (uma a cada HW_STEP s) */
#define HW_STEP 10
#define HW_NONE -1000       /* sem leitura */
#define HW_GPUS 4           /* placas de video com sensor */

struct k_hwstat {
  int sensor;               /* 0 nenhum, 1 AMD Tctl, 2 Intel DTS */
  int cpu_c;                /* agora (lido na hora) */
  int cpu_max_c, cpu_min_c; /* desde o boot */
  int ssd_c, ssd_max_c;     /* NVMe (log SMART); HW_NONE sem SSD */
  int load_pct;             /* todas as CPUs, ultimos HW_STEP s */
  int ncpu;
  uint32_t mem_total_mb, mem_free_mb;
  uint32_t uptime_s;
  uint32_t hot_count;       /* amostras com a CPU >= 90 C */
  int hist_n;               /* amostras validas em *_hist (mais nova no fim) */
  int step_s;
  signed char cpu_hist[HW_HIST];
  signed char ssd_hist[HW_HIST];
  signed char load_hist[HW_HIST];
  int gpu_n;                /* placas achadas (AMD Polaris, NVIDIA Pascal+) */
  int gpu_c[HW_GPUS];       /* graus C; HW_NONE sem leitura */
  int gpu_max_c[HW_GPUS];
  uint32_t gpu_id[HW_GPUS]; /* vendor << 16 | device */
  char gpu_name[HW_GPUS][16];
  signed char gpu_hist[HW_GPUS][HW_HIST];
  uint32_t pad[4];
};

void thermal_start(void);
/* Copia o estado; a temperatura da CPU e lida na hora. 0 ok, -1 sem dados. */
int thermal_stat(struct k_hwstat* out);
