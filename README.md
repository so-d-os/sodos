# SODOS

[Português](README.pt-BR.md) | **English**

A 64-bit educational operating system (*long mode*), freestanding, written in C and assembly. It boots straight from the BIOS (`os.img`); there is also a legacy Multiboot path for GRUB. It does not depend on the Linux libc. It has a DOS-style shell, a graphical desktop with windows, FAT16 with subdirectories, NE2000 networking with TCP/IP, an HTTP server with a remote-control API, a browser with TLS 1.3, its own C/C++ compiler that runs inside the system, an image viewer, an MJPEG video player and a TrueType font rasterizer.

SODOS is designed to be the first operating system that can be commanded by an artificial intelligence API.

![SODOS desktop at 1920×1080, with the Browser open on the home page](docs/desktop.png)

The desktop at 1920×1080 with the Browser open and the Manager behind it. The screenshot was taken by the OS itself, with `GET /win/printscreen` on port 8797.

The code is in `src13/`; this README is at the repository root.

The prompt looks like `C:\>`; `C:` is a RAM disk and `D:` is `disk.img` (FAT16). User programs are `.EXE` files in the **SOX** format — they are not ELF, and the 32-bit `hello.bin` that the boot still loads is not executed.

## An OS commanded by AI

The HTTP API on port 8797 is the control surface: an agent runs commands, opens and closes windows, moves the pointer, queries the open windows and captures the screen — without a keyboard, mouse or human session.

The project's goal is to be light enough to **run LLMs inside the OS itself** and, at the same time, to do everything a conventional operating system does — files, disks, networking, windows, compiler, programs — using the least resources possible.

The kernel is lean by design:

| Item | Footprint |
|---|---|
| Kernel (`kernel.bin`) | ~193 KB |
| Boot image (`os.img`) | ~198 KB |
| Kernel heap | 2 MB fixed (`0xA80000`–`0xC7FFFF`) |
| User heap | from the end of the kernel to the top of RAM (256 MB in QEMU) |
| Runtime dependencies | no libc, no POSIX; only the hardware |

That base — a ready control API, a kernel of a few hundred KB and no POSIX layer — is what makes it viable to embed the model in the system itself.

## Quick run

cd src13

run.bat

<img width="1093" height="184" alt="image" src="https://github.com/user-attachments/assets/17ab4d72-06ab-4494-9eb8-d06df9a92e9a" />

## Repository structure

| Path | Contents |
|---|---|
| `src13/` | Code, `Makefile`, working disks and the internal documentation |
| `README.md` | This file (English) |
| `README.pt-BR.md` | Portuguese version |
| `LICENSE` | GNU GPLv3 |

The shell commands below assume the repository root; when they need the code, they start with `cd src13` (or `cd src13/c_compiler`). The `make` targets run in `src13/`. Unless stated otherwise, file paths are relative to `src13/`.

## What's inside

- **64-bit kernel** in long mode, 2 MB paging, kernel heap and user heap, exception handler with register and stack dump.
- **Two-stage BIOS boot**: `boot.asm` (512-byte stage 1) loads `stage2.asm`, which uses E820, validates the target RAM, loads the kernel at `0x01000000` and hands over a versioned `BootInfo`; a legacy Multiboot path for GRUB is still present.
- **DOS shell** with 33 built-in commands, history, TAB, subdirectories and drive switching.
- **FAT12 (read) and FAT16 (read/write)**, MBR partitions, up to 4 ATA disks, subdirectories, cross-drive copy and RAM disk.
- **`win` desktop**: movable and resizable windows, taskbar, Start menu, icons, thumbnails, Notepad, Manager, Explorer in assembly and Command.
- **C/C++ compiler** (`cc` + `link`) that runs on Linux and inside the OS (`CC.EXE`, `LINK.EXE`), with its own libc, DLLs, relocation and verified *self-hosting*.
- **Browser** with HTTP/1.1, HTML, a subset of CSS and JavaScript, images, on-disk cache, search (DuckDuckGo/Google/Bing/Images) and TLS 1.3 with certificate verification.
- **Network**: NE2000 in polling mode, ARP, IPv4, ICMP, UDP (DNS) and TCP; fixed IP `10.0.2.15` on QEMU's SLIRP.
- **HTTP server on port 8797** with routes to run commands, list/close windows, move the mouse and take a BMP *printscreen* — the interface that lets an AI command the OS.
- **Media**: `IMG.DLL` (PNG, JPEG, GIF, ICO), `IMAGEVW.EXE`, `VIDPLAY.EXE` (MJPEG/RGB in AVI/MOV/MP4) and `FONTADV.EXE` with a TTF rasterizer.
- **Host tools**: `soxrun` (runs SOX `.EXE` on Linux), `mkthumb`, `mkvid.py`, tests and *self-host* comparison.
- **Debugging**: serial log on COM1, QEMU log, `gdb`, *trace* ring, exception dump and diagnostic commands (`free`, `df`, `dstat`, `dlls`).

## Requirements

The `Makefile` is in `src13/` and uses Unix shell syntax (`bash`). On Windows, run it from WSL (`src13/run.bat` calls `wsl make run`).

| Tool | Purpose |
|---|---|
| `gcc`, `ld`, `nasm`, `make` | build the kernel, 32-bit apps and tools |
| `qemu-system-x86_64` | run it (with `-vga std` and `-device usb-tablet`) |
| `mtools` (`mcopy`, `mdir`, `mmd`) | copy files into `disk.img` |
| `dosfstools` (`mkfs.fat`) and `util-linux` (`sfdisk`) | `make format-disk` |
| `genisoimage` or `mkisofs` | `make iso` |
| `grub-mkrescue`, `xorriso` | `make iso-grub` (Multiboot path) |
| `gcc -m32` on the host | `mkthumb`, used by `make disk-files` |

## Building and running

At the repository root, enter `src13` (where the `Makefile` is):

```bash
cd src13
make          # builds os.img
make run      # QEMU with os.img, disk.img and disk2.img
```

`make run` opens three IDE disks and forwards host port 8797 to the guest:

```text
qemu-system-x86_64 -m 256 -usb -device usb-tablet
  -vga std -netdev user,id=n0,hostfwd=tcp::8797-:8797 -device ne2k_pci,netdev=n0
  -drive format=raw,file=os.img,if=ide,index=0
  -drive format=raw,file=disk.img,if=ide,index=1
  -drive format=raw,file=disk2.img,if=ide,index=2
```

The USB tablet provides the absolute pointer position used by the desktop. All targets in the `make` table below run in `src13/`.

### `make` targets

| Target | Effect |
|---|---|
| `make` / `make all` | Builds `os.img` (boot + kernel + `app.bin`) |
| `make run` | QEMU with the three disks and port 8797 forwarded |
| `make run-fs` | Like `run`, but starts fullscreen with `zoom-to-fit=off` (avoids QEMU GTK stretching after `win`) |
| `make vbox` | Rebuilds `os.img`, updates the VirtualBox VM `SODOS` (`os.vdi`) and starts it |
| `make vbox-update` | Same as `vbox`, but does not start the VM |
| `make vbox VBOX_DISKS=1` | Also recreates `disk.vdi`/`disk2.vdi` from `disk.img`/`disk2.img` (discards changes made inside the VM) |
| `make run-log` | Same as `run`, COM1 to `qemu_serial.log` and QEMU trace to `qemu.log` |
| `make run-log-stdio` | COM1 to the terminal **and** to `qemu_serial.log` (via `tee`) |
| `make run-console` | QEMU in curses |
| `make run-gdb` | QEMU stopped on `:1234` (`-s -S`) |
| `make run-format` | Opens QEMU and prints the `fdisk`/`format`/`mount` commands |
| `make debug` | Rebuilds the kernel with `-O0 -g` (old target; links only part of the kernel) |
| `make iso` | `so.iso` El Torito from `os.img` |
| `make iso-grub` | Multiboot ISO (`so-grub.iso`) — legacy target, older than the current modules |
| `make install-usb` | Writes `os.img` to a device with `dd` (**overwrites the device**) |
| `make format-disk` | Formats `disk.img` as FAT16 on Linux |
| `make disk-files` | Copies compiler, libc, examples, fonts and `content/` to `D:` |
| `make clean` | Removes objects, `os.img` and `executables.c` |
| `make clean-all` | `clean` + `disk.img`, `disk2.img` and the ISOs |

`disk.img` (256 MB) and `disk2.img` (100 MB) are created automatically on the first run.

If QEMU GTK keeps the old fullscreen zoom when the guest switches from text mode to `win`'s 1920×1080, use `make run-fs` (1:1 fullscreen) or press `Ctrl+Alt+0` after `win` to restore the scale; `Ctrl+Alt+F` still toggles fullscreen.

### VirtualBox

On Windows/WSL, `make vbox` builds the images, creates/updates the VirtualBox VM `SODOS` (BIOS, 256 MB, VBoxSVGA, 1920×1080) and starts it. `make vbox-update` updates without starting. The script keeps `D:` and `disk2` VDIs; `VBOX_DISKS=1` recreates them from the project images and discards changes made inside the VM. A running VM is powered off before the disk update.

### `os.img` layout

| Sector | Contents |
|---|---|
| 0 | `boot.bin` (512-byte stage 1: LBA/CHS load of stage 2) |
| 1 … N | `stage2.bin` (E820, memory validation, kernel/app load, BootInfo) |
| N+1 … | `kernel.bin`, loaded at `0x01000000` (16 MiB) |
| after the kernel | `app.bin` (legacy 32-bit application; the 64-bit kernel does not run it) |

The kernel is linked at `0x01000000`; `stage2` checks the E820 map before loading it and reports which range failed. `tools/validate_layout.sh` aborts the build on size/address/overlap errors (including a kernel that no longer fits below the 256 MiB identity map). See [BOOT.md](src13/BOOT.md) for the full BootInfo contract.

### Debugging with GDB

```bash
cd src13
make run-gdb
# in another terminal
gdb -ex "target remote :1234" -ex "symbol-file kernel.elf" -ex "break kmain" -ex "c"
```

### ISO and VirtualBox

`make iso` builds `so.iso` with `os.img` as an El Torito boot image. `make iso-grub` builds a Multiboot ISO with `kernel_multiboot.elf`; that target predates the current split into modules (`module`, `sched`, `httpd`, commands) and may not link. VM instructions in [TESTAR_ISO.md](src13/TESTAR_ISO.md) and [VIRTUALBOX_SETUP.md](src13/VIRTUALBOX_SETUP.md).

## How the system boots

1. The BIOS loads sector 0 at `0x7C00`. Stage 1 (`boot.asm`) reads stage 2 (`stage2.asm`) to `0x7E00` via `INT 13h` (LBA with CHS fallback), passes the boot drive and kernel LBA at `0x4FF0`, and jumps to stage 2.
2. Stage 2 enables A20, collects the E820 map at `0x5200`, validates the kernel/app ranges against usable RAM, loads the kernel to `0x01000000` through a low bounce buffer plus a small protected-mode copy, fills `BootInfo` at `0x5000`, and calls `kernel_entry` with `EBX = BootInfo*`.
3. `kernel_entry.asm` checks for long mode, builds the page tables (128 2 MB pages = 256 MB identity plus the LFB), enables PAE + paging and calls `kmain(BootInfo*)`. Without long mode it writes `N` to the serial port and halts; a bad BootInfo writes `B`.
4. `kmain` validates BootInfo, zeroes the BSS, initializes memory (low `kmalloc` arena + E820-driven high `umalloc` arena after the kernel), the serial port (COM1), remaps and masks the PIC, builds the IDT, captures the BIOS 8×16 font, enters text mode, starts the network and HTTP, registers the commands, initializes the drives, mounts `D:` if there is FAT16, runs `HTTPD.EXE` if it exists on `D:` and opens the shell.

The kernel **does not enable interrupts** (`sti` is never called): keyboard, mouse, network, timer and disk are polled. The IDT exists to catch exceptions and IRQs, which are masked.

### Memory map

| Address | Use |
|---|---|
| `0x4FF0` | Stage 1 → stage 2 parameters (`boot_drive`, `kernel_lba`) |
| `0x5000` | `BootInfo` v1 (128 bytes) |
| `0x5200`–`0x57FF` | E820 map (up to 64 entries) |
| `0x7C00` / `0x7E00` | Stage 1 / stage 2 |
| `0x58000`–`0x5FFFF` | Slot for `asm`/`run`, assembly window programs and legacy `app.bin` |
| `0x7C000` | **NET1** gate for assembly (TCP) |
| `0x7E000` | FAT sector buffer (*bounce*) |
| `0x80000`–`0x806FF` | MBR/`fdisk` buffers |
| `0x80800` | FAT format buffer (absolute symbol, not in `kernel.bin`) |
| `0x9FC00` | Initial kernel stack |
| `0xB8000` | VGA text 80×25 |
| `0x100000`–`0x9FFFFF` | Screen in RAM (graphics back buffer) |
| `0xA00000`–`0xA59000` | Boot sector, FAT, root and open directory |
| `0xA80000`–`0xC7FFFF` | Kernel heap (2 MB) |
| `0x01000000` | Kernel (code, data, BSS) |
| after the kernel | User heap (E820-driven, capped at 256 MB in QEMU) |

### Scheduling

There is no preemption: each program runs until it returns control in `gui_event`, `gui_present`, network read or disk read. On the desktop, up to **8 programs** run as cooperative tasks, each with a 48 KB stack; the desktop switches between them and draws the windows.

## Disks and FAT

| Drive | What it is |
|---|---|
| `C:` | RAM disk: 7 embedded files + up to 16 created files (start at 1 KB, grow to 32 KB). Lost on power-off |
| `D:` | ATA 1 = `disk.img` (256 MB). Mounted automatically at boot if partition 0 is FAT12/FAT16 |
| other letters | Created with `mount` from any ATA/partition (or as a RAM disk) |

The kernel keeps **a single active FAT disk at a time**: mounting another one makes every FAT letter use the new disk. `C:` is always RAM.

`disk2.img` (ATA 2, 100 MB) is not mounted automatically.

### File system

- FAT12 and FAT16 for reading; writing, `md` and `rd` only on FAT16.
- FAT32, exFAT and long names (VFAT LFN) are not supported: names are **8.3**, shown in uppercase.
- Subdirectories up to 8 components of 12 characters, with `.` and `..`.
- Primary MBR partitions (4 entries); no extended, logical or GPT partitions.
- Writing: the FAT and root directory live in RAM and are written to disk on every change (both FAT copies), with no delayed cache.

| Limit | Value |
|---|---|
| FAT root | up to 64 KB; `format` creates 512 entries |
| Open directory | 32 KB (chain of up to 64 clusters) |
| Path | 8 components of up to 12 characters |
| `copy` | files up to 32 KB |
| `type` / `edit` | 2 KB per file |
| `cat` | 4 KB |
| `asm` | 16 KB source, 32 KB code, 64 labels |
| Clusters | from the BPB; `format` picks 8/16/32 KB to fit FAT16 |

`make format-disk` creates MBR + partition type `0x06` + FAT16 labelled `MYDISK` (8 KB clusters). It needs `sfdisk` and `mkfs.fat`; `disk-files` formats the disk itself if it has no FAT yet.

### Disk commands

Ways to mount:

```text
mount                 drive status
mount D:              RAM disk on D: (erases the FAT on that letter)
mount 1 D:            whole ATA 1 on D:
mount 1 D: 0          partition 0 of ATA 1 on D:
mount 2 E: 0          partition 0 of ATA 2 (disk2.img) on E:
mount 0               shortcut: partition 0 of ATA 1 on D:
```

Create from scratch inside the system:

```text
fdisk create 0 1 204799 6 1
format 0 MYDISK
mount 0
dir
```

> The partition type is parsed as hexadecimal **without** the `0x` prefix: use `6` or `06`. `0x06` is read as `0` and the command rejects the type. `make run-format` prints the example with `0x06`; change it to `6`. `format` always uses ATA 1 and only accepts partitions of type `0x04`/`0x06`. After `format`, mount again.

Preparing the disk on the host (with `sfdisk`, `mkfs.fat` and `mtools`):

```bash
cd src13
make format-disk          # MBR + partition type 0x06 + FAT16, label MYDISK
make -C c_compiler all    # cc, link, libc and the .EXE files
make disk-files           # copies everything to D:
make run
```

`disk-files` requires QEMU to be **closed**: the system rewrites the FAT on shutdown and would erase what `mcopy` wrote. To copy a single file:

```bash
mcopy -o -i src13/disk.img@@512 MEUAPP.C ::/MEUAPP.C
```

### Paths

`[X:][\][dir\...]name`, with `\` or `/`, upper or lower case. A bare `X:` switches the current drive. The prompt shows the current directory (`D:\USER\DEFAULT> `).

## Shell

The main loop reads a line and calls `shell_exec_line`. If the first token is a built-in command, it runs; otherwise the kernel looks for an `.EXE` — exact name or with `.EXE`, on the current drive and then on `D:` and `C:` — and calls `mod_exec`.

### Line editing

| Key | Effect |
|---|---|
| `Tab` | Completes the last token (commands, embedded executables and files on the drive) |
| `↑` / `PgUp` | History backward (32 lines) |
| `↓` / `PgDn` | History forward; past the end, clears the line |
| `←` / `→` | Moves the cursor |
| `Home` / `End` | Start / end of line |
| `Backspace` / `Del` | Deletes before / under the cursor |
| `Esc` | Clears the whole line |
| `Enter` | Runs it |

No Ctrl (Ctrl+C does not interrupt a command). The line is at most 255 characters; the tokenizer splits on spaces only and accepts up to 16 arguments. No wildcards, pipes or quotes.

### Built-in commands

| Command | Function |
|---|---|
| `help` | Lists the command names |
| `echo words...` | Prints the rest of the line; `echo text > FILE` writes a file to the RAM disk |
| `ls` | RAM-disk and embedded files (not the FAT disk) |
| `dir [path]` | FAT directory (marks `<DIR>`) |
| `cd [path]` | Enters a directory; with no argument shows the current directory |
| `md dir` | Creates a directory (FAT16 only) |
| `rd dir` | Removes an empty directory (FAT16 only) |
| `type file` | Shows a file from any drive (2 KB) |
| `cat file` | Shows a file from the mounted FAT drive (4 KB) |
| `copy source dest` | Copies between drives (32 KB); `copy c:hello.asm d:` |
| `clear` / `cls` | Clears the screen |
| `history` | Numbered history (session only) |
| `pause` | Waits for a key |
| `edit [file]` | Full-screen editor; `Esc` saves and exits (2 KB) |
| `load name` | Loads an old embedded binary: `hello`, `testapp`, `editor`. They are i386 and do not run on the 64-bit kernel |
| `asm file.asm` | Assembles a long-mode subset at `0x58000` and writes `NAME.BIN` next to it |
| `run` | Runs the code assembled by `asm` in this session |
| `mount` | Mounts RAM or FAT, or lists the drives |
| `fdisk list [drive]` / `fdisk create ...` | Lists and creates partitions |
| `format partition label` | Formats FAT16 (always ATA 1) |
| `disks` | Lists the ATA disks 0–3 |
| `df` | Total/used/free space of the mounted FAT |
| `free` | Kernel and user heaps |
| `dstat` / `ata` | Last ATA access (drive, LBA, status, error) |
| `visual` | 320×200 graphics demo (or VBE); `Esc` returns |
| `mouse` | PS/2 mouse test (position, buttons, raw packet); `Esc` exits |
| `win` / `win.exe` | Enters the graphical desktop; `Esc` returns to text |
| `ping host [-n count] [-l bytes]` | ICMP; e.g. `ping 10.0.2.2`, `ping example.com -n 2` |
| `dlls` | Loaded SOX modules |
| `shutdown` | Powers off the machine |

Aliases: `clear`/`cls`, `dstat`/`ata`, `win`/`win.exe`.

### Examples

```text
D:
dir
cd USER\DEFAULT\DESKTOP
dir
COPY C:\HELLO.ASM D:
```

```text
CC HELLO.C
HELLO um dois
```

## Desktop (`win`)

`win` enters the graphical environment; `Esc` returns to the text prompt (saving Notepad if it has changes).

### Display

- Bochs VBE 32 bpp: `1920x1080`, `1600x1200`, `1280x1024`, `1024x768`, `800x600`, `640x480`. The default is the largest mode that fits in video memory; without VBE it falls back to VGA 13h `320x200`.
- The UI was designed for 1024×768 and is scaled; on tall screens the font and cursor use scale 2.
- To change: Start menu → **Settings** → **Display**, or the **Display settings** window. The change restarts the desktop and closes the open windows. **Default** returns to automatic mode.

### Taskbar and menu

The bar has only the **Start** button and the `ESC=DOS` hint. The menu has 7 items:

| Item | What it opens |
|---|---|
| Notepad | Native text editor (opens `C:\readme.txt`) |
| Manager | Informative manager window |
| Explorer | `explorer.asm`, assembled at `0x58000` and run as a window app |
| Settings | Submenu **Display**, **Fonts** (VGA vs 8×8 preview) and **Advanced fonts** |
| Browser | `D:\BROWSER.EXE` |
| Command | Graphical prompt (several windows) |
| Exit | Back to DOS |

Right-clicking the background opens a context menu with **Console** and **Display settings**.

### Icons

The left column holds the fixed shortcuts **DOS** (Manager), **ARQ** (Notepad), **WEB** (Browser) and **CMD** (Command).

The other icons come from `D:\USER\DEFAULT\DESKTOP` (up to 24 items, no scrolling). A left click opens:

| Type | Action |
|---|---|
| Folder | Text window listing the contents (does not navigate) |
| `.EXE` / `.BIN` | Runs as a SOX program |
| `.PNG` `.JPG` `.JPE` `.GIF` `.ICO` `.BMP` | Opens in `IMAGEVW.EXE` (BMP is not decoded; see below) |
| `.MP4` `.MOV` `.M4V` `.AVI` `.MPG` `.MPE` `.MJG` `.MJP` `.3GP` `.MKV` `.WMV` `.FLV` `.OGV` | Opens in `VIDPLAY.EXE` |
| Others | Text window with the name |

Thumbnails: `make disk-files` generates `NAME.THM` (format `THM1`, up to 64×64 RGBA) for every image in `content/`; the desktop draws the thumbnail with transparency. Video files get a drawn red triangle; folders get a yellow icon.

### Windows

- Up to **12 windows** on the desktop and up to **8 programs** running at the same time (one per window).
- Dragging the title bar moves; dragging the bottom-right corner resizes (minimum 180×100 on a large screen); the `x` closes.
- Clicking brings to the front. There is no minimize, maximize, system menu or Alt+Tab.
- If the program opens a canvas window (`gui.h`), its text window is hidden.
- An `.EXE` typed in Command writes its output there and Command has no prompt until it finishes; the desktop and the other programs keep running. Without `win`, `k_gui_open` has nowhere to draw.

### Notepad

Loads `C:\readme.txt` on open. It has a **File** menu with **Save**, **Save as...** and **Open**, with a name box and a file list. `F2` saves; `Esc` closes the menu/dialog, or exits the desktop saving. Files up to 1 KB.

### Command

Black 80×40 window with the same shell interpreter. Several can be open; the `exit` command (only available there) closes the window.

### Other

- `visual` opens a separate graphics demo, without windows.
- `mouse` shows the raw PS/2 packet and the buttons.
- There is no *printscreen* key inside the OS; the capture is done over HTTP (`/win/printscreen`).

## System applications

`make disk-files` leaves these at the root of `D:`:

| File | What it is |
|---|---|
| `CC.EXE`, `LINK.EXE` | Compiler and linker that run inside the OS |
| `LIBC.DLL` | libc (stdio, stdlib, string, ctype) in long mode |
| `GUI.DLL` | Canvas window on top of `k_gui_*` |
| `IMG.DLL` | PNG, JPEG, GIF and ICO decoder |
| `TLS.DLL` | TLS 1.3 client |
| `BROWSER.EXE` | Browser |
| `COMMAND.EXE` | Inside `win` it becomes the Command window; in text mode it only explains |
| `IMAGEVW.EXE` | Image viewer |
| `VIDPLAY.EXE` | Video player |
| `HTTPD.EXE` | Shows the server routes; the kernel already starts the service at boot |
| `HTTPS.EXE` | GET over TLS (`HTTPS example.com /`) |
| `HELLO.EXE`, `HELLO2.EXE` | `argc`/`argv` examples |
| `CRT0.OBJ`, `KERNEL.IMP` | Entry point and kernel export list |
| `*.H`, `*.C`, `*.CPP` | Headers, libc, examples and the `cc`/`link` sources themselves |

`D:\SODOS\` holds `FONTADV.EXE` and `FONTS\` with four TTFs. `IMAGEVW.C` also goes to the root, as an example.

The `CALC`, `MATHX.DLL`, `USEMATH`, `DYNLOAD` and `VMAIN` examples are not shipped built; compile them inside the OS from the copied sources.

## Writing a program

A program is freestanding C compiled by this repository's `cc`. The visible entry point is `main`; `CRT0.OBJ` provides `_start`, which calls `main` and then `exit`.

```c
#include <stdio.h>

int main(int argc, char** argv) {
  int i;
  printf("argc=%d\n", argc);
  for (i = 0; i < argc; i++)
    printf("  argv[%d]=%s\n", i, argv[i]);
  return 0;
}
```

Inside the OS:

```text
D:
CC HELLO.C
HELLO um dois
CC -c HELLO.C -o HELLO.OBJ
LINK /out:HELLO.EXE CRT0.OBJ HELLO.OBJ LIBC.DLL KERNEL.IMP
```

`edit D:\HELLO.C`, `Esc` to save, `CC HELLO.C`.

### libc

`c_compiler/include/` has `stdio.h`, `stdlib.h`, `string.h`, `stdint.h`, `stdarg.h`, `stdbool.h`, `stddef.h`, `limits.h`, `assert.h`, `errno.h`, `ctype.h`, plus `so.h`, `gui.h`, `img.h` and `tls.h`.

- `stdio`: `printf`/`fprintf`/`sprintf`/`snprintf` and the `v*` variants, `fopen`/`fclose`, `fread`/`fwrite`, `fgetc`/`fgets`/`fputc`/`fputs`/`puts`, `fseek`/`ftell`/`rewind`, `feof`/`ferror`, `remove`, `perror`.
- `stdlib`: `malloc`/`calloc`/`realloc`/`free`, `exit`/`abort`/`atexit`, `atoi`/`strtol`/`strtoul`/`strtoll`/`strtoull`, `abs`, `rand`/`srand`, `qsort`, `bsearch`, `system`, `getenv`.
- `string`: `mem*`, `strlen`/`strnlen`, `strcpy`/`strncpy`, `strcat`/`strncat`, `strcmp`/`strncmp`/`strcasecmp`, `strchr`/`strrchr`/`strstr`/`strpbrk`/`strspn`/`strcspn`/`strtok`, `strdup`, `strerror`.
- `ctype`: `is*` and `to*`.

`printf` goes to the kernel console (text screen, or the Command window when the program was opened by `win`).

`fopen` in `"r"` reads the whole file into memory; `fclose` on a file opened in `"w"` or `"a"` writes the whole file at once. `fopen_mem` reads from a buffer. There is no real *streaming*.

### Kernel API (`so.h`)

Functions not wrapped by libc are imported from the `KERNEL` library (44 exports):

| Group | Functions |
|---|---|
| Console | `k_putc`, `k_puts`, `k_write`, `k_getc`, `k_readline`, `k_clear` |
| Files | `k_fsize`, `k_fload`, `k_fread`, `k_fsave`, `k_flist`, `k_drive`, `k_set_drive` |
| Memory | `k_malloc`, `k_free`, `k_realloc`, `k_msize`, `k_meminfo` |
| Process | `k_exit`, `k_exec`, `k_last_error` |
| DLL | `k_dll_load`, `k_dll_sym`, `k_dll_free` |
| Time | `k_rdtsc`, `k_ms`, `k_date`, `k_random` |
| Network | `k_net_ready`, `k_dns`, `k_tcp_connect`, `k_tcp_send`, `k_tcp_recv`, `k_tcp_close` |
| Window | `k_gui_open`, `k_gui_close`, `k_gui_title`, `k_gui_size`, `k_gui_font`, `k_gui_fill`, `k_gui_text`, `k_gui_present`, `k_gui_event`, `k_gui_pixels` |

`k_fload`/`k_fsave` read/write the whole file; `k_fread` reads a range starting at an offset (useful for large files). `k_msize` returns the allocated size; `k_meminfo` returns total and free.

The kernel calling convention is the OS's own: integer arguments in `edi`, `esi`, `edx`, `ecx`, `r8d`, `r9d`. Functions compiled by `cc` use the stack, 4 bytes at a time. The linker generates the *thunk*; the C program calls `k_puts` like a normal function.

### Graphical window (`gui.h` + `GUI.DLL`)

Only works for a program opened by `win`.

```c
#include <gui.h>

int main(void) {
  gui_event_t ev;
  if (gui_open("Hello")) return 1;
  gui_fill(0, 0, gui_width(), gui_height(), 0xC0C0C0);
  gui_text(8, 8, "click or close", -1, 0x000000, 0xC0C0C0, 0);
  gui_present();
  while (gui_wait(&ev, -1) != GUI_EV_CLOSE) {
    if (ev.type == GUI_EV_DOWN) {
      gui_fill(ev.x, ev.y, 4, 4, 0x000080);
      gui_present();
    }
  }
  gui_close();
  return 0;
}
```

```text
CC MYWIN.C GUI.DLL
```

| Function | Use |
|---|---|
| `gui_open` / `gui_close` / `gui_title` | Opens/closes/renames the window |
| `gui_width` / `gui_height` / `gui_char_w` / `gui_char_h` | Client size in pixels and the text cell size |
| `gui_fill`, `gui_frame`, `gui_text`, `gui_text_fit` | Rectangles, frames and text |
| `gui_pixels` | RGBA block (`R,G,B,A`; alpha < 128 is not drawn) |
| `gui_image` | Scales and clips an RGBA image |
| `gui_present` / `gui_wait` | Shows the frame / waits for an event |
| `gui_hit` / `gui_button` | Rectangle test and ready-made button |
| `gui_edit_*` | Single-line text box (512 bytes, caret, selection) |

Events: `GUI_EV_DOWN`, `GUI_EV_UP`, `GUI_EV_DRAG`, `GUI_EV_KEY`, `GUI_EV_CLOSE`, `GUI_EV_WHEEL`. Special keys: `GUI_KEY_UP`, `GUI_KEY_DOWN`, `GUI_KEY_LEFT`, `GUI_KEY_RIGHT`, `GUI_KEY_PGUP`, `GUI_KEY_PGDN`, `GUI_KEY_HOME`, `GUI_KEY_END`, `GUI_KEY_DEL`. `Enter` arrives as `'\r'`. Colors are `0xRRGGBB`. Text flags: `GUI_TEXT_NOBG`, `GUI_TEXT_BOLD`, `GUI_TEXT_UNDERLINE`.

`GUI.DLL` is a thin layer over 10 kernel exports (`k_gui_open`, `k_gui_close`, `k_gui_title`, `k_gui_size`, `k_gui_font`, `k_gui_fill`, `k_gui_text`, `k_gui_present`, `k_gui_event`, `k_gui_pixels`).

### Images (`IMG.DLL`)

```c
#include <img.h>
img_t im;
if (img_decode(data, len, &im) == 0) { /* im.rgba, im.w, im.h */ }
img_free(&im);
```

Decodes PNG (non-interlaced, up to 8 bits), JPEG (baseline and progressive), GIF (first frame, with transparency) and ICO (embedded BMP or PNG). It does not decode standalone BMP or animate GIF. It scales the output to at most 800 px per side. Used by `IMAGEVW.EXE`, the browser and `mkthumb`.

### TLS (`TLS.DLL`)

```c
#include <tls.h>
if (tls_connect("example.com", 443) == 0) {
  tls_send(req, n);
  n = tls_recv(buf, cap);
  tls_close();
}
tls_cert_ok();   /* 1 if the certificate was verified */
tls_security();  /* "HTTPS" or the warning */
```

TLS 1.3 only, cipher `TLS_AES_128_GCM_SHA256`, X25519 (P-256 only after a *HelloRetryRequest*), SNI and ALPN `http/1.1`. One session at a time. Verification covers the name, validity, server signature, a chain of up to 6 certificates and 121 embedded roots — but it **does not block** the connection: on failure, the warning text appears in the status bar.

### C++

The same `cc` accepts a small C++, enough to split a program into classes and several files. It is not g++'s C++: there are no templates, exceptions, overloading, `operator`, `virtual` methods, STL or global objects with constructors.

| Feature | Form |
|---|---|
| Class | `class Name { public: ... };`. The name becomes a type. `struct` remains C |
| Method | `void Name::method(int x) { ... }` or the body inside the class. Call: `obj.method(1)` and `p->method(1)`. Inside the method, the field may appear without `this->` |
| Constructor and destructor | `Name(int x)` and `~Name()`. `Name a(1);` and `Name b;` (if the constructor takes no argument) are locals. The destructor runs at the end of the block and before `return` |
| Inheritance | One base, `class B : public A`. The constructor calls the base with `: A(x)` or, if it takes no argument, on its own. The base destructor runs afterwards |
| `new` / `delete` | `new Name(1)`, `new int[n]`, `delete p`, `delete[] a`. `new[]` does not call the constructor |
| Reference | `void inc(int& x)`. The argument must be a variable |
| `namespace` | One level. `namespace mathx { int dbl(int x); }` and, from outside, `mathx::dbl(2)` |
| `bool`, `nullptr` | `bool` is `_Bool`. `nullptr` is a null pointer |

A method declared in the class and defined in another file links as a C function. Pass objects by pointer or reference: copying a class with a constructor does not call the copy constructor. Inside the system the name fits 8.3, hence `VEC.CPP` and `VMAIN.CPP`.

```text
CC VMAIN.CPP VEC.CPP
VMAIN
```

On the host:

```bash
cd src13/c_compiler
./build/cc -o out/VMAIN.EXE examples/vmain.cpp examples/vec.cpp
./build/soxrun -C out VMAIN.EXE
```

### SOX format

Defined in `c_compiler/sox.h`. Little-endian, magic `SOX1`, 16-`u32` header (64 bytes).

An `.EXE` or `.DLL` contains the header, the image (code + data), relocations, imports, exports and the string table. The loader adds the load base to every relocation and fills each import slot with the function address (`KERNEL`, `LIBC.DLL`, another DLL). The call goes through a `jmp [slot]` (`FF 25`).

| File | Role |
|---|---|
| `.EXE` | Program. Entry `_start(argc, argv)` |
| `.DLL` | Library. Optional entry `DllMain(reason)`; `SOX_FLAG_DLL` flag |
| `.OBJ` | COBJ object (`COB1`) from `cc` |
| `.LIB` | Static library (`CLB1`), several `.OBJ` files |
| `.IMP` | Export list. `KERNEL.IMP` is generated from `kernel/kexports.c` |
| `CRT0.OBJ` | `_start` for every C program |

The loader is in `kernel/module.c`: it loads the EXE, resolves the DLLs (each program gets its own copy, reference-counted), calls `_start` on its own stack (1 MB by default) and frees the memory on return. Limits: 32 loaded modules, 16 dependencies per module, 8 nested loads, program depth 16.

Errors: `MOD_ERR_NOTFOUND` (-1000), `MOD_ERR_FORMAT` (-1001), `MOD_ERR_MEMORY` (-1002), `MOD_ERR_IMPORT` (-1003), `MOD_ERR_DEPTH` (-1004).

### DLL

A function or global variable marked `__export` goes into the export table. In the bootstrap gcc the attribute is empty; use `link /export:name` or `/exportall`.

```c
/* mathx.c */
__export int mx_gcd(int a, int b) { /* ... */ }
```

In the OS:

```text
CC -dll MATHX.C
CC USEMATH.C MATHX.DLL
USEMATH
```

Load on demand, without linking the DLL into the EXE (`examples/dynload.c`):

```c
int h = k_dll_load("MATHX.DLL");
gcd_fn gcd = (gcd_fn)k_dll_sym(h, "mx_gcd");
k_dll_free(h);
```

Ready-made DLLs in `c_compiler/out/`: `LIBC.DLL`, `GUI.DLL`, `IMG.DLL`, `TLS.DLL`.

### Standalone assembly

```text
asm PROG.ASM
run
```

The assembler in `shell/cmd_asm.c` accepts: `org`, `db`/`dw`/`dd`, `nop`, `ret`, `int`, `push`/`pop`, `inc`/`dec`, `mov`, `add`/`or`/`and`/`sub`/`xor`/`cmp`, `jmp`/`call`/`je`/`jz`/`jne`/`jnz` and labels. It writes the bytes at `0x58000` and the flat binary next to the source. `push`/`pop`/`call`/`ret` are 64-bit; `org` must be `0x58000`.

The assembled code receives `puts_k` in the first argument register (`rdi`). It is a path separate from SOX `.EXE` files.

Embedded examples on `C:` (view with `type`):

- `hello.asm` — prints a phrase with `puts_k`.
- `curl.asm` — does a GET using the **NET1** gate at `0x7C000` (four 4-byte pointers to `connect`/`send`/`recv`/`close`).
- `explorer.asm` — `win` window app: receives a message, an API table (title, clear, item, name, status, select, drive, open) and its own state.

## Host compiler

The compiler that runs on Linux is `c_compiler/build/cc`. It produces `.OBJ` in the COBJ format and `link` produces SOX `.EXE`/`.DLL`. The standard libc (`LIBC.DLL`) also comes from this `cc`, in long mode. `gcc -m32` does not run on this kernel.

```bash
cd src13/c_compiler
make              # build/cc, build/link, out/LIBC.DLL, out/CC.EXE, examples and apps
make test         # tests/t_*.c, run by soxrun
make examples     # calc in two files, MATHX.DLL, dynload and VMAIN
make selfhost     # CC.EXE recompiles itself and the binary must come out identical
```

`make selfhost` compiles each `cc` module with `CC.EXE` running under `soxrun` and compares byte by byte with the objects produced by the host `cc`.

`cc` options:

```text
cc [-c] [-o output] [-dll] [-I dir] [-D name[=value]] [-L dir]
   [-E] [-w] [-v] [-nostdlib] files.c [.obj .lib .dll .imp]
```

Without `-c`, `cc` compiles each `.c` or `.cpp` and links with `CRT0.OBJ`, `LIBC.DLL` and `KERNEL.IMP`. `-dll` builds a DLL. `-E` only shows the preprocessed output. `-v` prints each source that became an `.OBJ`. `.cc`, `.cpp` and `.cxx` are the same language as `.c`.

`link` options:

```text
link [/out:FILE.EXE] [/dll] [/entry:name] [/export:name] [/exportall]
     [/stack:bytes] [/map] files.obj .lib .dll .imp
link /lib /out:MYLIB.LIB a.obj b.obj
```

Compiling your own file and putting it on the OS disk:

```bash
cd src13/c_compiler
./build/cc -o out/MYAPP.EXE examples/hello.c
cd ..
make disk-files
make run
```

`soxrun` runs a SOX `.EXE` on Linux, with the `-C` folder in place of the `D:` root:

```bash
cd src13/c_compiler
./build/soxrun -C out HELLO.EXE one "two three"
```

For a new program on the host, put the source in `c_compiler/examples/` (`disk-files` copies `*.c`, `*.h`, `*.cpp`) and run `make disk-files`.
## Network

The stack is in `net/net.c`: NE2000 (`ne2k_pci`) in polling mode, ARP, IPv4 with checksum, ICMP, UDP for DNS only and TCP.

- Fixed IP `10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3` (QEMU SLIRP). No DHCP.
- UDP: DNS A queries with EDNS, up to 4 answers, following CNAME.
- TCP: **one client connection** at a time, MSS 1460, 16 KB window, segments up to 1000 bytes, retransmission every 500 ms (8 tries), 8 s receive timeout. The HTTP server uses a separate connection.
- No IPv6, fragmentation, SACK, *window scaling* or congestion control.

```text
ping 10.0.2.2
ping example.com -n 2 -l 64
```

`ping` uses ICMP with a 2 s timeout between attempts and shows RTT and TTL. The browser and `HTTPS.EXE` use `k_tcp_*`.

## HTTP server

The kernel starts a server on port **8797** at boot, in parallel with the shell. With `make run`, the host reaches the guest at `http://127.0.0.1:8797/`.

It is through this API that an AI agent operates the machine: `GET /exec` and `/win/execute` run commands, `/win/mouse` and `/click` control the pointer, `/win/windows` and `/win/close` manage the windows and `/win/printscreen` returns the screen as BMP.

It is HTTP/1.0, one request per connection (`Connection: close`), and accepts `GET`, `POST`, `PUT`, `DELETE` and `HEAD`.

| Request | Effect |
|---|---|
| `GET /` | Lists the routes |
| `GET /win` | Lists only the desktop routes |
| `GET /win/printscreen` | Desktop screen as 24-bit BMP |
| `GET /win/mouse/coord` | Cursor position |
| `GET /win/mouse?x=10&y=20&click=1` | Moves the cursor; `click=1` presses and releases |
| `GET /win/windows` | Windows: `title x y w h` |
| `GET /win/close?program=Manager` | Closes the window (accepts `program` or `title`) |
| `GET /win/execute?cmd=dir` | Runs a shell line |
| `POST /win/execute` | The body is the command, or the `cmd` field |
| `GET /exec?cmd=help` | Same as execute |
| `POST /exec` | The body is the command |
| `POST /mouse` | `x`, `y`, `click` |
| `POST /click` | Click |

The `cmd` is the same prompt line (`help`, `dir`, `ping 10.0.2.2`, `HELLO`, …). `win` and `visual` refuse a second open while another API command is still running. Captured output is limited to 4608 bytes and the command to 240.

> **Warning**: there is no authentication. Anyone who reaches port 8797 can run commands, close windows and inject mouse events. `make run` publishes the port on `127.0.0.1` only; do not expose it on the network.

## Browser

`BROWSER.EXE` (opened from the Start menu, the WEB icon or `BROWSER [term]` at the prompt) combines HTTP, HTML, a little CSS and JavaScript, TLS, GUI and images.

### Interface

- One window, one document: bar with **Back**, **Reload**, address field and **Go**; no tabs, no forward, no bookmarks, no *find*, no zoom.
- History of 32 URLs; new navigation drops what was ahead.
- Status bar with progress and message; it turns brown when the certificate was not verified.
- `Enter` navigates; `Tab` switches address/page; `Esc` leaves the field or closes; `Backspace` goes back (on the page) and deletes (in a field).
- Scrolling: wheel (3 lines per step), `↑`/`↓`, `PgUp`/`PgDn`/`Space` (one screen) and `Home`/`End`. The wheel also scrolls while the next page loads.
- Any key or click during loading **interrupts** it and keeps the previous page.

### Addresses

- `https://`, `http://`, `about:` and `file:`. A host without a scheme tries HTTPS and falls back to HTTP if it fails.
- DOS paths (`D:FILE.HTM`, `C:\...`) become `file:`; `.htm`/`.html` render, images open as a picture and anything else appears as text.
- `javascript:`, `mailto:` and `tel:` do not navigate; `data:` works in images.
- 3xx redirects (with `Location`) up to 8 hops.
- No cookies, no form POST (GET only), no *download* to disk, no gzip, no proxy, no HTTP authentication.

### Search

Words in the address bar (or after `BROWSER` at the prompt) become a search when they contain a space or are a lone term without a dot/colon/slash. Prefixes:

| In the bar | Search |
|---|---|
| `black cat` or `d black cat` | DuckDuckGo (`html.duckduckgo.com`, no JavaScript) |
| `g google` | Google |
| `b black cat` | Bing |
| `i black cat` | Images: Bing thumbnail grid |

Google only answers browsers that run JavaScript, and DuckDuckGo sometimes asks for an anti-bot challenge, which this browser does not show. When one refuses, the search moves to the next (Google → DuckDuckGo → Bing), with a notice at the top. Search exit links (`/l/?uddg=`, `/url?q=`, `/ck/a?u=`) go straight to the destination. In image search, the thumbnail opens the original image (or the Bing copy, if the original does not open); the name below opens the source page.

### HTML, CSS and JavaScript

Flow layout on a character grid, with word wrap. Supported tags include `p`, `br`, `hr`, `h1`–`h6`, `pre`, `blockquote`, lists (`ul`/`ol`/`li`/`dl`), `table`/`tr`/`td`, `form`, `a`, `b`/`strong`, `i`/`em`, `u`, `code`, `img`, `input`, `title` and blocks (`div`, `section`, `header`, `footer`, `nav`, `figure`, `center`…).

- No CSS box model, `float`, positioning, `iframe` (`frameset` is not handled), `meta refresh` or charset detection (UTF-8 with Latin-1 fallback).
- Tables only chain text: `tr` breaks the line, `td`/`th` separate with two spaces; no `colspan`/`rowspan`.
- CSS: tag selectors, `.class`, `#id` and comma lists; properties `color`, `background(-color)`, `font-weight`, `text-decoration`, `display:none`, `width`/`height` in px, and `style=""`. Up to 256 rules, 8 external sheets, 96 KB per sheet, 256 KB total, one level of `@import`.
- JavaScript: `var`/`let`/`const`, `if`/`else`, `while` (200-iteration cap), `return`, arithmetic and comparisons, short strings, `alert()`, `document.getElementById()` and `el.style`/`innerHTML`/`innerText`. Up to 16 scripts of 64 KB per page, 32 variables, 24 ids. No events, arrays, objects, `for`, timers or network.
- Images: PNG, JPEG, GIF and ICO via `IMG.DLL`, at most 32 per page, scaled to 800 px per side. Every image in the document is downloaded (no *lazy loading*).
- Forms: text `input` and a submit button only, always via GET.

### Cache and connection

200 non-image pages go to `D:\temp\net\HHHHHHHH.NET` (FNV-1a of the URL, `SOCACHE1` header); there is no expiry or total limit, and the entry is checked against the URL to avoid collisions. **Reload** ignores and rewrites the cache. Search pages and pictures are never cached. The maximum body is 4 MB.

The connection to the server stays open during the page and is reused by pictures from the same host; it is closed at the end.

### TLS

TLS 1.3 with `TLS_AES_128_GCM_SHA256`, X25519/P-256, SNI and ALPN `http/1.1`. The chain is validated against 121 embedded roots, with name, validity, server signature (RSA-PSS or ECDSA P-256) and each link's signature (RSA PKCS#1 or ECDSA P-256/P-384). If validation fails, the page still loads and the status shows `HTTPS, certificate NOT verified: <reason>`.

### Limitations

No modern CSS, no real JavaScript, no cookies, no POST, no downloads, no tabs, no `iframe`, no animation. Google usually refuses; DuckDuckGo may ask for a captcha. Firefox does not fit here: it is large C++ with a POSIX system, a JavaScript engine and a graphics stack this kernel does not have — the OS browser remains `BROWSER.EXE`.

## Fonts and images

### Advanced fonts (`FONTADV.EXE`)

Opens from the Start menu → Settings → **Advanced fonts**, or directly:

```text
D:
SODOS\FONTADV.EXE
```

The list is fixed: four faces from `D:\SODOS\FONTS` (`NOTOSANS.TTF`, `NOTOSERF.TTF`, `NOTOMONO.TTF`, `SANSCOLL.TTF`). It shows name, full name, version, copyright, trademark and license (SIL OFL 1.1), and renders the typed phrase.

- `↑`/`↓` changes the font; `PgUp`/`PgDn` (or the `-`/`+` buttons, in steps of 2) change the size from 8 to 64 px.
- The text field accepts the phrase to draw.

The rasterizer (`fonts/ttfrast.c`) reads `cmap` format 4, `glyf`/`loca` (simple and composite outlines), `hmtx` and `head`, flattens the quadratic curves and produces 8-bit alpha at 2× with a 2×2 average. There is no *hinting* or *kerning*; filling is even-odd.

The WPF project in `fonts/FontesAvancadas/` is the window design prototype, which runs on Windows, not in the OS. `utils/getfonts.sh` downloads the Noto fonts and `utils/scan_font_lic.ps1` reads the TTF license.

### Images (`IMAGEVW.EXE`)

Opens from the desktop (click an image or thumbnail) or from the command line:

```text
D:
IMAGEVW D:\USER\DEFAULT\DESKTOP\LOGO.PNG
```

Fits the image to the window size, with transparency. `Esc`, `q` or the `x` close it. Formats: PNG (non-interlaced), JPEG (including progressive), GIF (first frame) and ICO. Limit of 4 MB per file; no zoom, pan, EXIF or animation. **BMP is not decoded** — the desktop lists `.BMP`, but the viewer only shows "could not open".

Desktop thumbnails: `c_compiler/build/mkthumb` (host) generates `NAME.THM` = `"THM1"` + 16-bit width/height + RGBA, longest side 48 px.

## Video

`VIDPLAY.EXE` (click a video on the desktop) plays **MJPEG and RGB** inside **AVI**, **MOV** and **MP4**, plus a JPEG sequence. There is no sound.

| Control | Action |
|---|---|
| `Space` | Pause / resume |
| `←` / `→` | Previous / next frame |
| `Home` | First frame |
| `Esc` / `q` / `x` | Close |

Up to 8000 frames per file. Samples in `content/`: `SAMPLE.AVI` (MJPEG 8×8, 3 frames), `SAMPLE.MP4` (JPEG, 3 frames) and `VIDEO2.MP4` (H.264, not playable).

Unsupported (the player shows the reason in the window): H.264 (`avc1`/`avc3`), HEVC (`hvc1`/`hev1`), MPEG-1/2, MKV/WebM, WMV/ASF, FLV and audio. The H.264 decoder in `utils/h264.c` (High 4:2:0, CABAC, I/P, *deblocking* only on luma) is work in progress, tested on the host by `utils/h264test.c` and **not integrated** into the player.

## Debugging and diagnostics

### Serial and logs

The kernel writes to COM1 (115200 8N1) with the prefixes `[kernel]`, `[net]`, `[mod]`, `[httpd]`, `[visual]`, `[mouse]` and `[exc]`.

| Target | Where it goes |
|---|---|
| `make run-log` | `qemu_serial.log` + `qemu.log` (`cpu_reset`, `guest_errors`, `int` trace) |
| `make run-log-stdio` | terminal + `qemu_serial.log` (line by line, via `tee`) |

`make run-log` deletes the previous logs and uses `-no-reboot -no-shutdown`; close QEMU so the file is fully flushed.

### Exceptions

An exception prints to the serial port and the screen: vector, error code, `RIP`, `RSP`, `RBP`, `CS`, `SS`, `DS`, `EFLAGS`, `CR2`, six stack words and a *backtrace* of up to 8 frames via `RBP`, followed by the trace ring (64 events) and `hlt`.

### Useful commands

```text
free      heaps and largest free block
df        FAT space
dstat     last ATA access
dlls      loaded modules, base, size and references
```

### Over HTTP

`GET /win/printscreen` returns the screen as BMP (useful to debug the desktop without a keyboard); `/win/windows`, `/win/mouse/coord` and `/win/execute?cmd=...` let you inspect and operate the machine from outside.

### Common problems

| Symptom | Likely cause |
|---|---|
| `Bad command or file name` | The `.EXE` is not on `D:`; run `make disk-files` with QEMU closed |
| `D:` does not mount at boot | `disk.img` has no FAT12/16 partition; run `make format-disk` |
| `fdisk create` rejects the type | Use `6`/`06`, not `0x06` |
| `dir` works and `HELLO` does not | Missing `LIBC.DLL`/`KERNEL.IMP`, or the `.EXE` is in another folder |
| Browser without network | `ne2k_pci` and `hostfwd` only exist with `make run`; fixed IP `10.0.2.15` |
| `make disk-files` erases files | QEMU was open; close it and repeat |
| Video does not open | Unsupported codec (H.264/HEVC/MPEG-1/2) |
| BMP image does not open | `IMG.DLL` does not decode BMP |
| Black screen in VirtualBox | Disable EFI and use BIOS; see [TESTAR_ISO.md](src13/TESTAR_ISO.md) |
| `make debug`/`iso-grub` fail | Old targets that link only part of the kernel; use `make` + `run-gdb` |

## Known limitations

- **No preemption** and no memory protection: everything runs in ring 0, in a single address space. An infinite loop in a program hangs the machine.
- **One active FAT at a time**; no FAT32, exFAT, LFN, extended partitions or GPT.
- **One TCP client at a time**; no DHCP, IPv6 or generic UDP sockets.
- **No audio**, no USB storage, no floppy, no DMA.
- **8.3 names** on FAT; files from `make disk-files` are copied in uppercase.
- **No Ctrl** in the shell; there is no way to interrupt a running command.
- **No access control**: the HTTP server on port 8797 runs commands without authentication.
- **No `reboot`**; `shutdown` uses ACPI/the Bochs port without parsing the ACPI tables.
- `IMG.DLL` does not decode BMP, although the desktop accepts `.BMP`.
- `make debug` and `make iso-grub` are outdated relative to the current kernel.
- Stage 2 still loads `app.bin` at `0x58000` for compatibility, but the 64-bit kernel ignores that 32-bit application.

## Directory map

Paths relative to `src13/`, except the first row:

| Folder or file | Contents |
|---|---|
| root | `README.md`, `README.pt-BR.md`, `LICENSE` (GPLv3) and `.git` |
| `kernel/` | `kmain`, boot, linker, IDT/ISR/IRQ/PIC, memory, scheduler, SOX loader, exports and the ATA driver (`disk.h`) |
| `fs/` | FAT12/16, MBR partitions, drives and files (`fat`, `partition`, `drives`, `files`) |
| `net/` | NE2000, ARP/IPv4/ICMP/UDP/TCP/DNS and the HTTP server on port 8797 |
| `video/` | Desktop, windows, mouse, cursor, VGA font and the graphics mode |
| `shell/` | Prompt commands (`cmd_*.c`) and `cmd_shutdown.asm` |
| `disks/` | `mount`, `dir`/`cd`/`md`/`rd`, `cat`, `fdisk`, `format`, `copy`, `disks`, `df`, `dstat` |
| `shell.h` | Prompt API at the root, for `shell/` and `disks/` |
| `c_compiler/cc/` | Compiler (`lex`, `pp`, `parse`, `gen`, `type`) |
| `c_compiler/link/` | SOX/COBJ linker |
| `c_compiler/lib/` | libc |
| `c_compiler/include/` | Headers (`so.h`, `stdio.h`, `gui.h`, `img.h`, `tls.h`) |
| `c_compiler/gui/`, `img/`, `tls/`, `browser/` | DLLs and the browser |
| `c_compiler/examples/` | `hello`, `calc`, `mathx`, `dynload`, `https`, `command`, `httpd`, `vmain`/`vec` |
| `c_compiler/tools/soxrun.c` | Runs SOX on Linux |
| `c_compiler/tests/` | `t_basic`, `t_io`, `t_pp`, `t_cpp`, `t_crypto` |
| `fonts/` | TTF rasterizer, `FONTADV`, sample fonts and the WPF prototype |
| `utils/` | `image_view`, `videoplay`, `mkthumb`, `h264*`, `getfonts.sh` |
| `content/` | Files that become desktop icons (and the video samples) |
| `DOCUMENTACAO.md` | Old notes on ports, IDT and the 32-bit kernel memory map |
| `TESTAR_ISO.md`, `VIRTUALBOX_SETUP.md`, `CONFIGURACAO_VBOX.txt` | How to test the ISO and set up the VM |
| `readme.txt`, `exemplo.md`, `teste.txt`, `history.log` | Files embedded in `C:` (`readme.txt` is what Notepad opens) |

## License

The repository ships the GNU GPLv3 in `LICENSE`, at the root. The fonts in `src13/fonts/ship/` are SIL Open Font License 1.1 (see `OFL.TXT`).

## Quick flow

1. `cd src13`
2. `make -C c_compiler all`
3. `make format-disk && make disk-files`
4. `make run`
5. At the prompt: `D:` and then `HELLO`, or `win` and the Start menu
6. For a new source already on `D:`: `CC MYAPP.C` and `MYAPP`
7. To see the desktop from outside: `http://127.0.0.1:8797/win` and `/win/printscreen`