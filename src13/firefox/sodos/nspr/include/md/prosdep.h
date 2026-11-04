/* prosdep.h (SODOS) - seleciona a configuracao de plataforma do NSPR.
 *
 * Sobrescreve pr/include/md/prosdep.h pelo include path: o NSPR e XP_UNIX,
 * mas o SO e o SODOS, nao o Linux/glibc. O _unixos.h e o _pth.h continuam
 * sendo os genericos do NSPR.
 */
#ifndef prosdep_h___
#define prosdep_h___

#include "prtypes.h"

PR_BEGIN_EXTERN_C

#include "md/_sodos.h"
#include "md/_unixos.h"
#include "md/_unix_errors.h"

#ifdef _PR_PTHREADS
#include "md/_pth.h"
#endif

PR_END_EXTERN_C

#endif /* prosdep_h___ */
