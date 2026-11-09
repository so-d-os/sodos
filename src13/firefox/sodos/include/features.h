/* features.h (SODOS) - so o que headers no estilo glibc pedem (scsi/sg.h).
 * Nao define __GLIBC__: o SODOS nao e glibc. */
#ifndef _FEATURES_H
#define _FEATURES_H
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#ifndef __THROW
#define __THROW
#endif
#endif
