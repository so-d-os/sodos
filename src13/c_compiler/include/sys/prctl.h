#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H
#ifdef __cplusplus
extern "C" {
#endif
#define PR_SET_NAME 15
#define PR_GET_NAME 16
int prctl(int option, ...);
/* lote3 */
#define PR_SET_DUMPABLE 4
#define PR_GET_DUMPABLE 3
#define PR_SET_PTRACER 0x59616d61

/* lote6 */
#define PR_CAPBSET_READ 23

#ifdef __cplusplus
}
#endif
#endif
