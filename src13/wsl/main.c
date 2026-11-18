/* main.c - WSL.EXE: o "WSL2" do SODOS.
 *
 * Le o Ubuntu do WSL2 direto do SSD do Windows (NTFS -> ext4.vhdx -> ext4),
 * so leitura, e sobe o mesmo Ubuntu numa VM AMD-V com o kernel do proprio
 * WSL (C:\Program Files\WSL\tools\kernel). Ver wsl/README.md.
 *
 *   WSL list | info | ls /x | cat /x | cp /x D:\DEST
 *   WSL run [-m MB] [-w MB] [-i INIT] [-k KERNEL] [-c "params"] [-t s] [-x "texto"] [-v] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sodos_api.h"
#include "wslcore.h"
#include "wslcli.h"
#include "vmm/vmm.h"

static int nvme_rd(void* ctx, uint64_t lba, uint32_t count, void* buf) {
  (void)ctx;
  return k_nvme_raw_read(lba, count, buf) ? -1 : 0;
}

static int save_local(const char* path, const void* data, uint32_t len) {
  return k_fsave(path, (const char*)data, (int)len) < 0 ? -1 : 0;
}

static int exists_ext4(ext4_fs* fs, const char* p) {
  ext4_inode in;
  return fs && ext4_lookup(fs, p, 1, &in) == 0;
}

/* 1 se o arquivo e um script (#!). O /init das distros WSL e o binario da
 * Microsoft (precisa de Hyper-V/vsock): so usamos /init se for script. */
static int is_script(ext4_fs* fs, const char* p) {
  ext4_inode in;
  char b[2];
  return fs && ext4_lookup(fs, p, 1, &in) == 0 && in.size > 2 &&
         ext4_pread(fs, &in, 0, b, 2) == 0 && b[0] == '#' && b[1] == '!';
}

static int cmd_run(int argc, char** argv, const wsl_io* disk) {
  vmm_config cfg;
  wsl_stack st;
  wsl_distro info;
  const char* which = 0;
  const char* init = 0;
  const char* kpath = 0;
  const char* extra = "";
  static char cmdline[2048];
  uint8_t* kbuf = 0;
  uint64_t ksize = 0;
  int i, rc;
  memset(&cfg, 0, sizeof cfg);
  cfg.ram_mb = 1024;
  cfg.cow_mb = 512;
  cfg.cpu = -1;
  for (i = 2; i < argc; i++) {
    const char* a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : 0;
    if (!strcmp(a, "-m") && v) cfg.ram_mb = (uint32_t)atoi(v), i++;
    else if (!strcmp(a, "-w") && v) cfg.cow_mb = (uint32_t)atoi(v), i++;
    else if (!strcmp(a, "-i") && v) init = v, i++;
    else if (!strcmp(a, "-k") && v) kpath = v, i++;
    else if (!strcmp(a, "-c") && v) extra = v, i++;
    else if (!strcmp(a, "-t") && v) cfg.timeout_s = (uint32_t)atoi(v), i++;
    else if (!strcmp(a, "-x") && v) {
      /* \n \r \t \\ como no C (a linha do shell nao tem quebra) */
      static char inp[1024];
      int o = 0;
      const char* s = v;
      while (*s && o < (int)sizeof inp - 1) {
        if (s[0] == '\\' && s[1]) {
          char e = s[1];
          inp[o++] = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;
          s += 2;
        } else {
          inp[o++] = *s++;
        }
      }
      inp[o] = 0;
      cfg.input = inp;
      i++;
    }
    else if (!strcmp(a, "-a") && v) cfg.input_after = v, i++;
    else if (!strcmp(a, "-d") && v) which = v, i++;
    else if (!strcmp(a, "-p") && v) cfg.cpu = atoi(v), i++;
    else if (!strcmp(a, "-v")) cfg.verbose = 1;
    else {
      printf("WSL run: opcao desconhecida %s\n", a);
      return 1;
    }
  }
  {
    char why[128];
    if (vmm_check(why, sizeof why)) {
      printf("WSL: %s\n", why);
      return 1;
    }
  }
  if (wsl_open_stack(disk, which, &st, &info)) {
    printf("WSL: %s\n", wsl_err);
    return 1;
  }
  printf("WSL: distro %s (usuario %s), disco virtual de %llu MB\n", info.name, info.user,
         (unsigned long long)(vhdx_size(st.vd) >> 20));
  if (vhdx_dirty(st.vd))
    printf("WSL: aviso: o vhdx tem log pendente (o Windows nao fechou o WSL); o ext4 pode ter\n"
           "     journal por reaplicar, o Linux faz isso na camada de escrita em RAM.\n");

  /* kernel: arquivo local (-k D:\...) ou o do WSL no NTFS */
  if (kpath && kpath[0] && kpath[1] == ':') {
    int n = k_fsize(kpath);
    if (n <= 0 || !(kbuf = (uint8_t*)k_hmalloc((uint64_t)n, 4096)) ||
        k_fload(kpath, (char*)kbuf, n) != n) {
      printf("WSL: nao consegui ler %s\n", kpath);
      return 1;
    }
    ksize = (uint64_t)n;
  } else {
    const char* np = kpath ? kpath : wsl_find_kernel(st.ntfs);
    ntfs_file* kf = np ? ntfs_open(st.ntfs, np) : 0;
    if (!kf) {
      printf("WSL: kernel do WSL nao encontrado (%s); use -k\n", np ? wsl_err : "Program Files\\WSL\\tools\\kernel");
      return 1;
    }
    ksize = ntfs_fsize(kf);
    if (ksize > (128u << 20) || !(kbuf = (uint8_t*)k_hmalloc(ksize, 4096)) ||
        ntfs_pread(kf, 0, kbuf, (uint32_t)ksize)) {
      printf("WSL: erro lendo o kernel: %s\n", wsl_err);
      return 1;
    }
    printf("WSL: kernel %s (%llu KB)\n", np, (unsigned long long)(ksize >> 10));
    ntfs_close(kf);
  }

  if (!init) {
    if (is_script(st.fs, "/init")) init = "/init";
    else if (exists_ext4(st.fs, "/bin/bash")) init = "/bin/bash";
    else init = "/bin/sh";
  }
  {
    /* Shell como init: monta o basico antes (o "login" do WSL e direto como
     * root). Os argumentos depois de "--" vao para o init. */
    const char* sh_args = "";
    size_t il = strlen(init);
    if ((il >= 5 && !strcmp(init + il - 5, "/bash")) || (il >= 3 && !strcmp(init + il - 3, "/sh")))
      sh_args = " -- -c \"mount -t proc proc /proc;mount -t sysfs sys /sys;"
                "mount -t devtmpfs dev /dev 2>/dev/null;mkdir -p /dev/pts /run;mount -t devpts devpts /dev/pts;"
                "mount -t tmpfs tmpfs /run;mount -t tmpfs tmpfs /tmp;hostname sodos-wsl;"
                "export HOME=/root TERM=vt100;cd /root;exec $0 -l\"";
    snprintf(cmdline, sizeof cmdline,
             "console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 nolapic noapic acpi=off "
             "nokaslr tsc=reliable mitigations=off nosmp maxcpus=1 nr_cpus=1 "
             "pci=conf1,nomsi root=/dev/vda rw rootfstype=ext4 rootwait %s init=%s%s",
             extra, init, sh_args);
  }
  cfg.kernel = kbuf;
  cfg.kernel_len = (uint32_t)ksize;
  cfg.cmdline = cmdline;
  cfg.disk = st.disk;
  rc = vmm_run(&cfg);
  wsl_close_stack(&st);
  return rc;
}

int main(int argc, char** argv) {
  wsl_io disk;
  uint64_t secs;
  if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "/?")) {
    wsl_usage();
    return argc < 2;
  }
  secs = k_nvme_raw_sectors();
  if (!secs) {
    printf("WSL: nenhum SSD NVMe encontrado (o Windows precisa estar num NVMe)\n");
    return 1;
  }
  wsl_disk_io(&disk, nvme_rd, 0, secs);
  if (!strcmp(argv[1], "run")) return cmd_run(argc, argv, &disk);
  return wsl_cli_read(argc, argv, &disk, save_local);
}
