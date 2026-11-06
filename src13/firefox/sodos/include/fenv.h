/* fenv.h (SODOS) - declaracoes, valores do x86_64. */
#ifndef _FENV_H
#define _FENV_H
#ifdef __cplusplus
extern "C" {
#endif
#define FE_INVALID 0x01
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW 0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT 0x20
#define FE_ALL_EXCEPT 0x3d
#define FE_TONEAREST 0
#define FE_DOWNWARD 0x400
#define FE_UPWARD 0x800
#define FE_TOWARDZERO 0xc00
typedef unsigned short fexcept_t;
typedef struct {
  unsigned int cw;
  unsigned int mxcsr;
} fenv_t;
#define FE_DFL_ENV ((const fenv_t*)-1)
int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t* flag, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t* flag, int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int mode);
int fegetenv(fenv_t* env);
int feholdexcept(fenv_t* env);
int fesetenv(const fenv_t* env);
int feupdateenv(const fenv_t* env);
#ifdef __cplusplus
}
#endif
#endif
