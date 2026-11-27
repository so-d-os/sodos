#!/bin/sh
# build-image.sh - Linux proprio do WSL do SODOS (sem Windows/NTFS).
#
# Gera em wsl/build/image/ (copiar para D:\WSL\ do SODOS):
#   DEBIAN.IMG  ext4 com Debian 12 minimo (bash, git, curl, python3...) e os
#               modulos de rede/9p do kernel abaixo
#   VMLINUZ     kernel (o deste Linux, /boot; virtio_pci embutido)
#   INITRD.IMG  initrd pequeno: busybox + virtio_blk, monta /dev/vda e passa
#               para o /sbin/sodos-init da imagem
#
# Tudo sem root. Sem newuidmap (pacote uidmap, setuid) o mmdebstrap nao roda
# em --mode=unshare, e o fakechroot nao acha o chroot; entao:
#   1. mmdebstrap --mode=chrootless --variant=extract dentro do fakeroot: so
#      desempacota (dono 0 no tar) e o --extract-hook guarda os .deb em
#      /sodos-debs;
#   2. mke2fs -d dentro de "unshare -r" (dono 0 na imagem);
#   3. um boot no QEMU deste PC (root de verdade na VM) roda o dpkg -i dos
#      .deb (scripts dos pacotes, base de dados do dpkg), cria o /etc/passwd
#      e apaga /sodos-debs. Mesmo kernel e initrd que o WSL.EXE usa.
# mmdebstrap, busybox-static e fakeroot vem de "apt-get download" em ~/wsltools.
#
#   sh wsl/scripts/build-image.sh            (MB=2000 tamanho da imagem)
#   KVER=6.10.7-policorp-amd64 sh ...        (outro kernel de /boot)
#   KIMG="/mnt/c/Program Files/WSL/tools/kernel" sh ...
#                                            (bzImage fora do /boot; o WSL2
#                                            nao tem /boot, so /lib/modules)
#   FRESH=1 sh ...                         (baixa os pacotes de novo)
#   PARTS=0 sh ...                           (um DEBIAN.IMG so, MB < 2048)
# Saida em partes DEBIAN.000, .001... de 128 MB (o WSL.EXE novo le assim; o
# k_fsize do SODOS e int, entao um arquivo so fica abaixo de 2 GB). Com partes
# o MB pode passar de 2048. A imagem ja vem com gcc 12, binutils, libc6-dev,
# make, nasm e e2fsprogs (compilar o SODOS na VM, sem o FERRAM.SH).
# Para so acrescentar pacotes a imagem atual, sem este PC: nova-imagem.sh.
set -e
S=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$S/build/image}
W=${WORK:-$HOME/wsltools/image-work}
T=$HOME/wsltools
KVER=${KVER:-$(uname -r)}
MB=${MB:-2000}
PARTS=${PARTS:-1}
[ "$PARTS" = 1 ] || [ "$MB" -lt 2048 ] || { echo "PARTS=0 pede MB < 2048 (k_fsize do SODOS e int)"; exit 1; }
SUITE=${SUITE:-bookworm}
MIRROR=${MIRROR:-http://deb.debian.org/debian}
PKGS=bash,bash-completion,coreutils,procps,iproute2,iputils-ping,kmod,less,nano,ca-certificates,curl,wget,git
PKGS=$PKGS,openssh-client,python3,file,xz-utils,unzip,util-linux,dpkg,apt,base-files,base-passwd,debianutils
PKGS=$PKGS,init-system-helpers,login,passwd,sed,grep,tar,gzip,findutils,diffutils,hostname,ncurses-base
PKGS=$PKGS,ncurses-bin,perl-base,mawk,dash,usrmerge,libc-bin,sysvinit-utils,mount
# compilar o SODOS na VM (kernel, c_compiler, DLLs, WSL.EXE)
PKGS=$PKGS,gcc,cpp,binutils,libc6-dev,make,nasm,e2fsprogs

mkdir -p "$OUT" "$W" "$T/debs"
if [ ! -x "$T/mm/usr/bin/mmdebstrap" ] || { [ ! -x "$T/bb/bin/busybox" ] && [ ! -x "$T/bb/usr/bin/busybox" ]; } || [ ! -x "$T/fk/usr/bin/faked-sysv" ]; then
  (cd "$T/debs" && apt-get download mmdebstrap busybox-static fakeroot libfakeroot)
  mkdir -p "$T/mm" "$T/bb" "$T/fk"
  dpkg -x "$T"/debs/mmdebstrap_*.deb "$T/mm"
  dpkg -x "$T"/debs/busybox-static_*.deb "$T/bb"
  dpkg -x "$T"/debs/fakeroot_*.deb "$T/fk"
  dpkg -x "$T"/debs/libfakeroot_*.deb "$T/fk"
fi
BB=$T/bb/bin/busybox
[ -x "$BB" ] || BB=$T/bb/usr/bin/busybox
# cpio (initrd): o WSL2 nao tem; vem de "apt-get download cpio" em ~/wsltools/cp
if ! command -v cpio > /dev/null; then
  if [ ! -x "$T/cp/usr/bin/cpio" ]; then
    (cd "$T/debs" && apt-get download cpio) && dpkg -x "$T"/debs/cpio_*.deb "$T/cp"
  fi
  PATH=$T/cp/usr/bin:$PATH
fi
M=/lib/modules/$KVER
KIMG=${KIMG:-/boot/vmlinuz-$KVER}
[ -f "$KIMG" ] && [ -d "$M" ] || { echo "kernel $KVER nao achado ($KIMG e $M); KIMG= aponta o bzImage"; exit 1; }

# 1. Debian minimo (tar so desempacotado + os .deb em /sodos-debs)
[ -n "$FRESH" ] && rm -f "$W/rootfs.tar"
# chave do Debian: num Ubuntu (WSL2) o apt so tem as do Ubuntu e o mmdebstrap
# para em "NO_PUBKEY"; o pacote debian-archive-keyring fica em ~/wsltools/kr
KR=$T/kr/usr/share/keyrings/debian-archive-keyring.gpg
if [ ! -f /usr/share/keyrings/debian-archive-keyring.gpg ] && [ ! -f "$KR" ]; then
  (cd "$T/debs" && apt-get download debian-archive-keyring) && dpkg -x "$T"/debs/debian-archive-keyring_*.deb "$T/kr"
fi
KEYOPT=
[ -f /usr/share/keyrings/debian-archive-keyring.gpg ] || KEYOPT=--keyring=$KR
if [ ! -s "$W/rootfs.tar" ]; then
  PATH="$T/fk/usr/bin:$T/mm/usr/bin:/usr/sbin:/sbin:$PATH" \
    "$T/fk/usr/bin/fakeroot-sysv" -l "$T/fk/usr/lib/x86_64-linux-gnu/libfakeroot/libfakeroot-sysv.so" \
    -f "$T/fk/usr/bin/faked-sysv" -- \
    mmdebstrap --mode=chrootless --variant=extract $KEYOPT --include="$PKGS" \
    --extract-hook='mkdir -p "$1/sodos-debs"; cp "$1"/var/cache/apt/archives/*.deb "$1/sodos-debs/"' \
    "$SUITE" "$W/rootfs.tar" "$MIRROR" > "$W/mmdebstrap.log" 2>&1 || { tail "$W/mmdebstrap.log"; exit 1; }
fi

# 2. scripts da imagem
cat > "$W/sodos-init" <<'INIT'
#!/bin/sh
# /sbin/sodos-init - init do WSL do SODOS (a VM nao tem systemd nem o /init
# da Microsoft). Monta o basico, liga a rede pelo sodos.ip= da linha do
# kernel e roda o bash no console ttyS0; quando o bash sai, a VM desliga.
# Com sodos.service=1 (SET WSL=1 no SODOS) liga o agente de comandos na
# ttyS1 e o bash do console volta quando sai: a VM so para com SET WSL=0.
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /dev/pts /run /tmp
mount -t devpts devpts /dev/pts 2>/dev/null
mount -t tmpfs tmpfs /run
mount -t tmpfs tmpfs /tmp
hostname sodos-wsl
rows=25 cols=80 ip="" service=0
for a in $(cat /proc/cmdline); do
  case "$a" in
    sodos.service=*) service=${a#sodos.service=} ;;
    sodos.rows=*) rows=${a#sodos.rows=} ;;
    sodos.cols=*) cols=${a#sodos.cols=} ;;
    sodos.ip=*) ip=${a#sodos.ip=} ;;
  esac
done
ip link set lo up
# rede: sodos.ip=IP/MASCARA,GATEWAY,DNS (o NAT do WSL.EXE)
if [ -n "$ip" ]; then
  modprobe virtio_net 2>/dev/null
  addr=$(echo "$ip" | cut -d, -f1); gw=$(echo "$ip" | cut -d, -f2); dns=$(echo "$ip" | cut -d, -f3)
  ip link set eth0 up 2>/dev/null && ip addr add "$addr" dev eth0 && ip route add default via "$gw"
  rm -f /etc/resolv.conf
  echo "nameserver $dns" > /etc/resolv.conf
fi
# unidades do SODOS (virtio-9p "sodos" do WSL.EXE): /mnt/sodos/d -> /d e /mnt/d
modprobe 9pnet_virtio 2>/dev/null
modprobe 9p 2>/dev/null
mkdir -p /mnt/sodos
# Como no Git Bash (o Claude Code espera): /d, /z... sao as unidades (bind:
# o pwd -P mostra /d/...); /mnt/d aponta para /d, como no WSL.
if mount -t 9p -o trans=virtio,version=9p2000.L,msize=262144 sodos /mnt/sodos 2>/dev/null; then
  for d in /mnt/sodos/*; do
    l=$(basename "$d")
    [ -d "$d" ] || continue
    mkdir -p "/$l" && mount --bind "$d" "/$l"
    ln -sfn "/$l" "/mnt/$l"
  done
fi
export HOME=/root TERM=linux LANG=C.UTF-8
cd /root
stty rows "$rows" cols "$cols" </dev/ttyS0 2>/dev/null
if [ "$service" = 1 ]; then
  setsid /usr/local/sbin/sodos-agent </dev/null >/dev/null 2>/run/sodos-agent.log &
  while :; do
    setsid -w -c /bin/bash -l <>/dev/ttyS0 >&0 2>&0
    echo "(o bash saiu; a VM continua ligada para os comandos do SODOS - SET WSL=0 desliga)" >/dev/ttyS0
  done
fi
setsid -w -c /bin/bash -l <>/dev/ttyS0 >&0 2>&0
sync
# sem systemd/sysvinit nao ha poweroff: o do busybox chama reboot() direto
/usr/local/bin/busybox poweroff -f
echo o > /proc/sysrq-trigger
sleep 10
INIT

cat > "$W/sodos-agent" <<'AGENT'
#!/usr/bin/python3
# /usr/local/sbin/sodos-agent - comandos do SODOS pela ttyS1 (SET WSL=1).
# Protocolo (wsl/wslbox.h no SODOS):
#   SODOS -> Linux:  R <id> <flags> <cwdlen> <cmdlen>\n<cwd><cmd>  |  K <id>\n
#   Linux -> SODOS:  READY\n | O <id> <len>\n<bytes> | E ... | X <id> <codigo>\n
import os, signal, subprocess, threading, time, tty

fd = os.open("/dev/ttyS1", os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
wlock = threading.Lock()
procs = {}
buf = bytearray()

def send(b):
    with wlock:
        v = memoryview(b)
        while v:
            n = os.write(fd, v)
            v = v[n:]

def read_exact(n):
    global buf
    while len(buf) < n:
        d = os.read(fd, 65536)
        if not d:
            raise SystemExit(0)
        buf += d
    out = bytes(buf[:n])
    del buf[:n]
    return out

def read_line():
    global buf
    while b"\n" not in buf:
        d = os.read(fd, 65536)
        if not d:
            raise SystemExit(0)
        buf += d
    i = buf.index(b"\n")
    line = bytes(buf[:i])
    del buf[:i + 1]
    return line.decode("latin-1").strip()

def pump(stream, kind, rid):
    while True:
        try:
            d = os.read(stream.fileno(), 16384)
        except OSError:
            d = b""
        if not d:
            break
        send(("%s %s %d\n" % (kind, rid, len(d))).encode() + d)

# O agente nasce com "&" do sodos-init: um shell sem job control deixa o
# SIGINT e o SIGQUIT ignorados nos processos de fundo, e os filhos herdam.
# Sem isto o K (Ctrl+C, TerminateProcess do bash do Claude Code) nao matava
# nada: "sleep 60" ia ate o fim.
def child_setup():
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.signal(signal.SIGQUIT, signal.SIG_DFL)

# K: SIGINT no grupo; quem ainda estiver vivo leva SIGTERM em 2 s e SIGKILL
# em mais 3 s (returncode: o wait() do run() segura a trava do poll())
def reap(p):
    for sig, wait in ((signal.SIGTERM, 2.0), (signal.SIGKILL, 3.0)):
        t = 0.0
        while t < wait and p.returncode is None:
            time.sleep(0.1)
            t += 0.1
        if p.returncode is not None:
            return
        try:
            os.killpg(p.pid, sig)
        except OSError:
            return

def run(rid, flags, cwd, cmd):
    env = dict(os.environ)
    env.update(HOME="/root", LANG="C.UTF-8", TERM="dumb",
               PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")
    if not cwd or not os.path.isdir(cwd):
        cwd = "/root"
    args = ["/bin/bash", "-l", "-c", cmd] if flags & 1 else ["/bin/bash", "-c", cmd]
    try:
        p = subprocess.Popen(args, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True,
                             preexec_fn=child_setup)
    except Exception as e:
        send(("E %s %d\n" % (rid, len(str(e)) + 1)).encode() + (str(e) + "\n").encode())
        send(("X %s 127\n" % rid).encode())
        return
    procs[rid] = p
    t1 = threading.Thread(target=pump, args=(p.stdout, "O", rid), daemon=True)
    t2 = threading.Thread(target=pump, args=(p.stderr, "E", rid), daemon=True)
    t1.start(); t2.start()
    code = p.wait()
    t1.join(); t2.join()
    procs.pop(rid, None)
    if code < 0:
        code = 128 - code
    send(("X %s %d\n" % (rid, code)).encode())

send(b"READY\n")
while True:
    h = read_line().split()
    if not h:
        continue
    if h[0] == "R" and len(h) == 5:
        rid, flags, cl, ml = h[1], int(h[2]), int(h[3]), int(h[4])
        cwd = read_exact(cl).decode("utf-8", "replace")
        cmd = read_exact(ml).decode("utf-8", "replace")
        threading.Thread(target=run, args=(rid, flags, cwd, cmd), daemon=True).start()
    elif h[0] == "K" and len(h) == 2:
        p = procs.get(h[1])
        if p:
            try:
                os.killpg(p.pid, signal.SIGINT)
            except OSError:
                pass
            threading.Thread(target=reap, args=(p,), daemon=True).start()
AGENT

cat > "$W/sodos-setup" <<'SETUP'
#!/bin/sh
# primeiro boot (QEMU do PC que gerou a imagem): instala os pacotes de verdade
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH DEBIAN_FRONTEND=noninteractive
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /dev/pts /run /tmp; mount -t devpts devpts /dev/pts; mount -t tmpfs tmpfs /run
echo "sodos-setup: dpkg -i $(ls /sodos-debs | wc -l) pacotes"
dpkg --install --force-depends --force-confnew /sodos-debs/*.deb > /root/setup.log 2>&1
dpkg --configure -a --force-depends >> /root/setup.log 2>&1
dpkg --configure -a >> /root/setup.log 2>&1
echo "sodos-setup: dpkg rc=$? instalados: $(dpkg -l | grep -c '^ii')"
[ -s /etc/passwd ] || cp /usr/share/base-passwd/passwd.master /etc/passwd
[ -s /etc/group ] || cp /usr/share/base-passwd/group.master /etc/group
passwd -d root >/dev/null 2>&1
update-ca-certificates >/dev/null 2>&1
rm -rf /sodos-debs /var/cache/apt/archives/*.deb
echo "sodos-setup: $(bash --version | head -1)"
echo "sodos-setup: $(git --version)"
echo SODOS-SETUP-OK
sync
/usr/local/bin/busybox poweroff -f
echo o > /proc/sysrq-trigger
sleep 10
SETUP

# initrd: busybox + virtio_blk
rm -rf "$W/initrd" && mkdir -p "$W/initrd/bin" "$W/initrd/dev" "$W/initrd/proc" "$W/initrd/sys" "$W/initrd/root" "$W/initrd/lib/modules"
cp "$BB" "$W/initrd/bin/busybox"
# (o kernel do WSL2 tem o virtio_blk embutido: sem o .ko, o insmod so falha)
cp "$M/kernel/drivers/block/virtio_blk.ko" "$W/initrd/lib/modules/" 2>/dev/null || true
cat > "$W/initrd/init" <<'RD'
#!/bin/busybox sh
# initrd do WSL do SODOS: carrega o virtio_blk, monta /dev/vda e passa para o
# init da imagem (init= da linha do kernel; padrao /sbin/sodos-init).
B=/bin/busybox
$B mount -t proc proc /proc
$B mount -t sysfs sys /sys
$B mount -t devtmpfs dev /dev
$B insmod /lib/modules/virtio_blk.ko
init=/sbin/sodos-init
for a in $($B cat /proc/cmdline); do
  case "$a" in init=*) init=${a#init=} ;; esac
done
n=0
while [ ! -b /dev/vda ] && [ $n -lt 50 ]; do $B sleep 0.1; n=$((n + 1)); done
if ! $B mount -t ext4 -o rw /dev/vda /root; then
  echo "initrd: nao montei /dev/vda"
  exec $B sh
fi
$B umount /sys /proc
$B mount --move /dev /root/dev 2>/dev/null || $B umount /dev
exec $B switch_root /root "$init" "$@"
RD
chmod 755 "$W/initrd/init"
(cd "$W/initrd" && find . | cpio -o -H newc 2>/dev/null | gzip -9) > "$OUT/INITRD.IMG"
rm -f "$OUT/VMLINUZ" # o do /mnt/c vem sem permissao de escrita
cp "$KIMG" "$OUT/VMLINUZ"

# 3. ext4: extrai, acrescenta e grava como "root" num namespace
rm -f "$OUT/DEBIAN.IMG"
cp "$BB" "$OUT/../busybox.tmp"
unshare -r sh -e -c '
W=$1; M=$2; KVER=$3; OUT=$4; MB=$5
rm -rf "$W/root"; mkdir -p "$W/root"
tar -xf "$W/rootfs.tar" -C "$W/root" --no-same-owner --exclude="./dev/*" 2>/dev/null || true
install -m 755 "$W/sodos-init" "$W/root/sbin/sodos-init"
install -m 755 "$W/sodos-setup" "$W/root/sbin/sodos-setup"
install -D -m 755 "$W/sodos-agent" "$W/root/usr/local/sbin/sodos-agent"
install -D -m 755 "$4/../busybox.tmp" "$W/root/usr/local/bin/busybox"
echo sodos-wsl > "$W/root/etc/hostname"
printf "/dev/vda / ext4 rw 0 1\n" > "$W/root/etc/fstab"
mkdir -p "$W/root/lib/modules/$KVER"
for f in kernel/drivers/block/virtio_blk.ko kernel/drivers/net/virtio_net.ko kernel/drivers/net/net_failover.ko \
         kernel/net/core/failover.ko kernel/lib/dim/dimlib.ko kernel/fs/9p/9p.ko kernel/net/9p/9pnet.ko \
         kernel/net/9p/9pnet_virtio.ko kernel/fs/netfs/netfs.ko kernel/drivers/char/virtio_console.ko; do
  [ -f "$M/$f" ] || continue # kernel do WSL2: virtio e 9p embutidos
  mkdir -p "$W/root/lib/modules/$KVER/$(dirname $f)"
  cp "$M/$f" "$W/root/lib/modules/$KVER/$f"
done
cp "$M/modules.order" "$M/modules.builtin" "$M/modules.builtin.modinfo" "$W/root/lib/modules/$KVER/" 2>/dev/null || true
/sbin/depmod -b "$W/root" "$KVER"
/usr/sbin/mke2fs -q -t ext4 -L SODOSWSL -d "$W/root" "$OUT/DEBIAN.IMG" "${MB}M"
' sh "$W" "$M" "$KVER" "$OUT" "$MB"
rm -f "$OUT/../busybox.tmp"

# 4. primeiro boot no QEMU: dpkg de verdade (root na VM)
timeout 900 qemu-system-x86_64 -accel kvm -cpu host -m 2048 -nographic -no-reboot \
  -kernel "$OUT/VMLINUZ" -initrd "$OUT/INITRD.IMG" \
  -append "console=ttyS0 root=/dev/vda rw init=/sbin/sodos-setup panic=-1" \
  -drive file="$OUT/DEBIAN.IMG",format=raw,if=virtio > "$W/setup.log" 2>&1 < /dev/null || true
grep -a "sodos-setup\|SODOS-SETUP-OK\|Kernel panic\|initrd:" "$W/setup.log" | tail -8
grep -aq SODOS-SETUP-OK "$W/setup.log" || { echo "build-image: o primeiro boot falhou (ver $W/setup.log)"; exit 1; }
if [ "$PARTS" = 1 ]; then
  rm -f "$OUT"/DEBIAN.[0-9][0-9][0-9]
  split -b 128M -d -a 3 "$OUT/DEBIAN.IMG" "$OUT/DEBIAN."
  rm -f "$OUT/DEBIAN.IMG"
fi
ls -la "$OUT"
