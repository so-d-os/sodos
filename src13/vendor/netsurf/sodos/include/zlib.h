/* zlib.h - so a leitura gz* sem compressao (arquivos texto simples). */
#ifndef _SODOS_ZLIB_H
#define _SODOS_ZLIB_H
#include <stddef.h>
typedef void* gzFile;
typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;
gzFile gzopen(const char* path, const char* mode);
char* gzgets(gzFile f, char* buf, int len);
int gzclose(gzFile f);
#define Z_NULL 0
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR (-3)
#define Z_MEM_ERROR (-4)
#define Z_BUF_ERROR (-5)
#define Z_NO_FLUSH 0
#define Z_FINISH 4
#define MAX_WBITS 15
typedef struct z_stream_s {
  const Bytef* next_in;
  uInt avail_in;
  uLong total_in;
  Bytef* next_out;
  uInt avail_out;
  uLong total_out;
  const char* msg;
  void* state;
  void* zalloc;
  void* zfree;
  void* opaque;
  int data_type;
  uLong adler;
  uLong reserved;
} z_stream;
typedef z_stream* z_streamp;
int inflateInit2_(z_streamp s, int bits, const char* ver, int size);
#define inflateInit2(s, b) inflateInit2_((s), (b), "1.2.11", (int)sizeof(z_stream))
int inflate(z_streamp s, int flush);
int inflateEnd(z_streamp s);
#endif
