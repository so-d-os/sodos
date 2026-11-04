/* prcpucfg.h - config de CPU do NSPR para o alvo SODOS x86-64 x32 (ILP32).
 *
 * Substitui o prcpucfg.h que o build do NSPR gera a partir de _linux.cfg.
 * No SODOS a ABI e x32: long e ponteiro tem 4 bytes, int64 tem 8 e alinha
 * em 8. E o mesmo modelo do _linux.cfg para __i386__, mas em long mode.
 */
#ifndef nspr_cpucfg___
#define nspr_cpucfg___

#ifndef XP_UNIX
#define XP_UNIX
#endif

#define IS_LITTLE_ENDIAN 1
#undef IS_BIG_ENDIAN

#define PR_BYTES_PER_BYTE 1
#define PR_BYTES_PER_SHORT 2
#define PR_BYTES_PER_INT 4
#define PR_BYTES_PER_INT64 8
#define PR_BYTES_PER_LONG 4
#define PR_BYTES_PER_FLOAT 4
#define PR_BYTES_PER_DOUBLE 8
#define PR_BYTES_PER_WORD 4
#define PR_BYTES_PER_DWORD 8

#define PR_BITS_PER_BYTE 8
#define PR_BITS_PER_SHORT 16
#define PR_BITS_PER_INT 32
#define PR_BITS_PER_INT64 64
#define PR_BITS_PER_LONG 32
#define PR_BITS_PER_FLOAT 32
#define PR_BITS_PER_DOUBLE 64
#define PR_BITS_PER_WORD 32
#define PR_BITS_PER_DWORD 64

#define PR_BITS_PER_BYTE_LOG2 3
#define PR_BITS_PER_SHORT_LOG2 4
#define PR_BITS_PER_INT_LOG2 5
#define PR_BITS_PER_INT64_LOG2 6
#define PR_BITS_PER_LONG_LOG2 5
#define PR_BITS_PER_FLOAT_LOG2 5
#define PR_BITS_PER_DOUBLE_LOG2 6
#define PR_BITS_PER_WORD_LOG2 5
#define PR_BITS_PER_DWORD_LOG2 6

#define PR_ALIGN_OF_SHORT 2
#define PR_ALIGN_OF_INT 4
#define PR_ALIGN_OF_LONG 4
#define PR_ALIGN_OF_INT64 8
#define PR_ALIGN_OF_FLOAT 4
#define PR_ALIGN_OF_DOUBLE 8
#define PR_ALIGN_OF_POINTER 4
#define PR_ALIGN_OF_WORD 4
#define PR_ALIGN_OF_DWORD 8

#define PR_BYTES_PER_WORD_LOG2 2
#define PR_BYTES_PER_DWORD_LOG2 3

#define PR_AF_INET6 10

/* LP64 (SOX2): ponteiro/long de 8 bytes. O mesmo header serve x32 e x64;
 * o -m64 define __SIZEOF_POINTER__=8. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#ifndef IS_64
#define IS_64 1
#endif
#undef PR_BYTES_PER_LONG
#undef PR_BITS_PER_LONG
#undef PR_BITS_PER_LONG_LOG2
#undef PR_BYTES_PER_WORD
#undef PR_BITS_PER_WORD
#undef PR_BITS_PER_WORD_LOG2
#undef PR_BYTES_PER_WORD_LOG2
#undef PR_ALIGN_OF_LONG
#undef PR_ALIGN_OF_POINTER
#undef PR_ALIGN_OF_WORD
#define PR_BYTES_PER_LONG 8
#define PR_BITS_PER_LONG 64
#define PR_BITS_PER_LONG_LOG2 6
#define PR_BYTES_PER_WORD 8
#define PR_BITS_PER_WORD 64
#define PR_BITS_PER_WORD_LOG2 6
#define PR_BYTES_PER_WORD_LOG2 3
#define PR_ALIGN_OF_LONG 8
#define PR_ALIGN_OF_POINTER 8
#define PR_ALIGN_OF_WORD 8
#endif

#endif /* nspr_cpucfg___ */
