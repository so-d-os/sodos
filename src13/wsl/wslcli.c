/* wslcli.c - Comandos de leitura do WSL (iguais no SODOS e no host de teste):
 *   list              distros e kernel achados nas particoes NTFS
 *   info              detalhes da distro (vhdx, ext4)
 *   ls  /caminho      pasta do Ubuntu
 *   cat /caminho      arquivo do Ubuntu na tela
 *   cp  /caminho DEST copia um arquivo do Ubuntu para DEST (arquivo local)
 * -d NOME escolhe a distro (prefixo do nome da pasta). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wslcore.h"
#include "wslcli.h"

static int ls_cb(void* arg, const char* name, uint32_t ino, int type) {
  ext4_fs* fs = (ext4_fs*)arg;
  ext4_inode in;
  char t = '?';
  if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
  if (ext4_iget(fs, ino, &in) == 0) {
    switch (in.mode & EXT4_S_IFMT) {
    case EXT4_S_IFDIR: t = 'd'; break;
    case EXT4_S_IFREG: t = '-'; break;
    case EXT4_S_IFLNK: t = 'l'; break;
    default: t = 'c';
    }
    printf("%c%03o %5u %5u %12llu  %s", t, in.mode & 0777, in.uid, in.gid,
           (unsigned long long)in.size, name);
    if (t == 'l') {
      char tg[256];
      if (ext4_readlink(fs, &in, tg, sizeof tg) >= 0) printf(" -> %s", tg);
    }
    printf("\n");
  } else {
    printf("?         %s (tipo %d)\n", name, type);
  }
  return 0;
}

static int list_parts(const wsl_io* disk) {
  wsl_part parts[WSL_MAX_PARTS];
  int np = wsl_ntfs_parts(disk, parts, WSL_MAX_PARTS), i, k, total = 0;
  printf("Disco: %llu MB, %d particao(oes) NTFS\n", (unsigned long long)(disk->size >> 20), np);
  for (i = 0; i < np; i++) {
    wsl_sub sub;
    wsl_io pio;
    ntfs_vol* v;
    wsl_distro ds[WSL_MAX_DISTROS];
    const char* kp;
    int nd;
    wsl_sub_io(&sub, disk, parts[i].off, parts[i].size, &pio);
    v = ntfs_mount(&pio);
    if (!v) {
      printf("  particao %d: %s\n", parts[i].index, wsl_err);
      continue;
    }
    printf("  particao %d: NTFS \"%s\", %llu MB\n", parts[i].index, ntfs_label(v),
           (unsigned long long)(parts[i].size >> 20));
    kp = wsl_find_kernel(v);
    if (kp) printf("    kernel do WSL: %s\n", kp);
    nd = wsl_find_distros(v, ds, WSL_MAX_DISTROS);
    for (k = 0; k < nd; k++) {
      printf("    distro %s (usuario %s): %llu MB\n      %s\n", ds[k].name, ds[k].user,
             (unsigned long long)(ds[k].size >> 20), ds[k].path);
      total++;
    }
    ntfs_umount(v);
  }
  if (!total) printf("Nenhuma distro WSL encontrada.\n");
  return total ? 0 : 1;
}

void wsl_usage(void) {
  printf("WSL - le o Ubuntu do WSL2 direto do disco do Windows (so leitura)\n"
         "  WSL list               distros e kernel nas particoes NTFS\n"
         "  WSL info               detalhes da distro\n"
         "  WSL ls /caminho        lista pasta do Ubuntu\n"
         "  WSL cat /caminho       mostra arquivo\n"
         "  WSL cp /caminho DEST   copia arquivo do Ubuntu para o SODOS\n"
         "  WSL run [opcoes]       sobe o Ubuntu numa VM (AMD-V)\n"
         "  -d NOME                escolhe a distro (prefixo da pasta)\n"
         "  WSL exec comando...    roda no Linux da VM ao fundo (liga com SET WSL=1)\n"
         "  SET WSL=1 / SET WSL=0  VM ao fundo (console no Ctrl+F10) / desliga\n"
         "  BASH.EXE -c \"cmd\"     o WSL.EXE com esse nome: bash do Claude Code\n");
}

/* ls/cat/cp num ext4 ja montado (distro do Windows ou a imagem propria). */
int wsl_cli_ext4(int argc, char** argv, ext4_fs* fs, wsl_save_fn save) {
  const char* cmd = argv[1];
  int rc = 0;
  {
    ext4_inode in;
    const char* path = argc > 2 ? argv[2] : "/";
    if (!fs) {
      printf("WSL: ext4 ilegivel: %s\n", wsl_err);
      rc = 1;
    } else if (ext4_lookup(fs, path, 1, &in)) {
      printf("WSL: %s\n", wsl_err);
      rc = 1;
    } else if (!strcmp(cmd, "ls")) {
      if ((in.mode & EXT4_S_IFMT) == EXT4_S_IFDIR) rc = ext4_readdir(fs, &in, ls_cb, fs) < 0;
      else ls_cb(fs, path, in.ino, 0);
    } else if ((in.mode & EXT4_S_IFMT) != EXT4_S_IFREG) {
      printf("WSL: %s nao e arquivo\n", path);
      rc = 1;
    } else if (!strcmp(cmd, "cat")) {
      static char buf[65536];
      uint64_t off = 0;
      while (off < in.size) {
        uint32_t n = in.size - off > sizeof buf ? (uint32_t)sizeof buf : (uint32_t)(in.size - off);
        if (ext4_pread(fs, &in, off, buf, n)) {
          printf("\nWSL: %s\n", wsl_err);
          rc = 1;
          break;
        }
        fwrite(buf, 1, n, stdout);
        off += n;
      }
    } else {
      char* b;
      if (argc < 4) {
        printf("uso: WSL cp /caminho DESTINO\n");
        rc = 1;
      } else if (in.size > 0x7FFFFFFF || !(b = (char*)malloc((size_t)in.size + 1))) {
        printf("WSL: arquivo grande demais\n");
        rc = 1;
      } else {
        if (ext4_pread(fs, &in, 0, b, (uint32_t)in.size) || save(argv[3], b, (uint32_t)in.size)) {
          printf("WSL: falhou: %s\n", wsl_err);
          rc = 1;
        } else {
          printf("%s -> %s (%llu bytes)\n", path, argv[3], (unsigned long long)in.size);
        }
        free(b);
      }
    }
  }
  return rc;
}

int wsl_cli_read(int argc, char** argv, const wsl_io* disk, wsl_save_fn save) {
  const char* which = 0;
  const char* cmd;
  wsl_stack st;
  wsl_distro info;
  int rc = 0, i, j;
  /* tira -d NOME */
  for (i = 1, j = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-d") && i + 1 < argc) which = argv[++i];
    else argv[j++] = argv[i];
  }
  argc = j;
  if (argc < 2) {
    wsl_usage();
    return 1;
  }
  cmd = argv[1];
  if (!strcmp(cmd, "list")) return list_parts(disk);
  if (wsl_open_stack(disk, which, &st, &info)) {
    printf("WSL: %s\n", wsl_err);
    return 1;
  }
  if (!strcmp(cmd, "info")) {
    printf("Distro %s (usuario %s)\n  %s\n", info.name, info.user, info.path);
    printf("  vhdx: %llu MB no NTFS, disco virtual de %llu MB, blocos de %u KB%s\n",
           (unsigned long long)(info.size >> 20), (unsigned long long)(vhdx_size(st.vd) >> 20),
           vhdx_block_size(st.vd) >> 10,
           vhdx_dirty(st.vd) ? " (log pendente: o Windows nao fechou o WSL)" : "");
    if (st.fs) {
      ext4_inode in;
      printf("  ext4 \"%s\", %llu MB\n", ext4_label(st.fs),
             (unsigned long long)(ext4_bytes(st.fs) >> 20));
      if (ext4_lookup(st.fs, "/etc/os-release", 1, &in) == 0 && in.size < 4096) {
        char* b = (char*)malloc((size_t)in.size + 1);
        if (b && ext4_pread(st.fs, &in, 0, b, (uint32_t)in.size) == 0) {
          char* p = strstr(b, "PRETTY_NAME=");
          b[in.size] = 0;
          if (p) {
            char* e = strchr(p, '\n');
            if (e) *e = 0;
            printf("  %s\n", p);
          }
        }
        free(b);
      }
    } else {
      printf("  ext4: %s\n", wsl_err);
    }
  } else if (!strcmp(cmd, "ls") || !strcmp(cmd, "cat") || !strcmp(cmd, "cp")) {
    rc = wsl_cli_ext4(argc, argv, st.fs, save);
  } else {
    wsl_usage();
    rc = 1;
  }
  wsl_close_stack(&st);
  return rc;
}
