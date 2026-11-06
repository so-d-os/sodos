/* sys/statfs.h (SODOS) - declaracoes. */
#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H
#ifdef __cplusplus
extern "C" {
#endif
struct statfs {
  long f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  struct {
    int __val[2];
  } f_fsid;
  long f_namelen, f_frsize, f_flags, f_spare[4];
};
int statfs(const char* path, struct statfs* buf);
int fstatfs(int fd, struct statfs* buf);
#ifdef __cplusplus
}
#endif
#endif
