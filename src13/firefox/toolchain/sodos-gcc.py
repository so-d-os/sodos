#!/usr/bin/env python3
"""sodos-cc.py - driver C/C++ x86_64-sodos (compila x32 e liga SOX).

O gcc/g++ normal nao sabe produzir um executavel do SODOS: o loader carrega
imagens SOX (endereco 0, relocacoes de 32 bits) e o linker e o `link` do
c_compiler. Este driver faz o papel de `x86_64-sodos-gcc`:

  - compila .c/.cpp com sodos-cc/sodos-cxxlib (gcc -mx32, headers do SODOS);
  - no link, chama c_compiler/build/link com x_crt0.o e as DLLs/imports.

Assim o configure do Firefox (e qualquer build Unix) consegue rodar os
`conftest` de compilacao/link, mesmo sem um linker ELF de destino.
"""
import os
import glob
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # src13/firefox
SRC13 = os.path.dirname(ROOT)         # src13
C = os.path.join(SRC13, "c_compiler")
LINK = os.path.join(C, "build", "link")
LINK64 = os.path.join(C, "build", "link64")
GCC_INCLUDE = subprocess.check_output(["gcc", "-print-file-name=include"], text=True).strip()
CRT = os.path.join(C, "build", "x_crt0.o")
CRT64 = os.path.join(C, "build", "x_crt0_64.o")
LIBCX = os.path.join(C, "out", "LIBCX.DLL")
KERNEL = os.path.join(C, "out", "KERNEL.IMP")
POSIXX = os.path.join(C, "out", "POSIXX.DLL")
SODOS = os.path.join(ROOT, "build", "out", "SODOS.DLL")
LIBCXX = os.path.join(ROOT, "build", "libcxx", "LIBCXX.LIB")
CXXRT = os.path.join(ROOT, "build", "cxx", "cxx_runtime.o")
CXXRT2 = os.path.join(ROOT, "build", "libcxx", "cxx_runtime.o")
FP = os.path.join(ROOT, "build", "libcxx", "fpconv.o")
MATH = os.path.join(ROOT, "build", "libcxx", "mathx.o")
SODOS_INCLUDE = os.path.join(ROOT, "sodos", "include")
SYSROOT_LIB = os.path.join(SRC13, "libs", "dso", "build", "sysroot", "lib")
CC = os.path.join(ROOT, "sodos", "sodos-cc")
CXX = os.path.join(ROOT, "sodos", "sodos-cxxlib")
CXX64 = os.path.join(ROOT, "sodos", "sodos-cxxlib64")
CXXRT64 = os.path.join(ROOT, "build", "libcxx64", "cxx_runtime64.o")
INIT64 = os.path.join(ROOT, "build", "libcxx64", "init_array_stub64.o")
OVR64 = os.path.join(ROOT, "build", "libcxx64", "override_stub64.o")
PORT64 = os.path.join(ROOT, "build", "libcxx64", "port")
POSIX64 = os.path.join(ROOT, "build", "libcxx64", "posix64.o")


def is_cxx():
    return os.environ.get("SODOS_CC_KIND") == "cxx" or "++" in os.path.basename(sys.argv[0])


def is64():
    return os.environ.get("SODOS_CC_64") == "1"


def host_version():
    exe = "g++" if is_cxx() else "gcc"
    try:
        # Saida inteira: o meson so reconhece o gcc pela linha do copyright
        # ("Free Software Foundation").
        return subprocess.check_output([exe, "--version"], text=True).rstrip("\n")
    except Exception:
        return "gcc (SODOS) 13.3.0\nCopyright (C) Free Software Foundation, Inc."


def print_info(arg):
    if arg == "--version":
        print(host_version())
        print("SODOS x86_64 target driver")
        return 0
    if arg in ("-dumpversion", "--version-only"):
        print("13.3.0")
        return 0
    if arg in ("-dumpmachine", "-print-target-triple"):
        print("x86_64-pc-sodos")
        return 0
    if arg.startswith("-print-file-name="):
        print(arg.split("=", 1)[1])
        return 0
    if arg.startswith("-print-prog-name="):
        print(arg.split("=", 1)[1])
        return 0
    return None


def run(cmd, **kw):
    if os.environ.get("SODOS_CC_DEBUG"):
        sys.stderr.write("+ " + " ".join(cmd) + "\n")
    return subprocess.call(cmd, **kw)


def expand_response_files(args):
    """O configure testa arquivos de lista do linker (@arquivo).

    O link64 nao le @file; expandimos os tokens aqui. -Wl,@file tambem vira
    os objetos/opcoes de linker contidos no arquivo."""
    out = []
    for a in args:
        path = None
        if a.startswith("@") and len(a) > 1:
            path = a[1:]
        elif a.startswith("-Wl,@") and len(a) > 5:
            path = a[5:]
        if path and os.path.isfile(path):
            try:
                with open(path, "r", errors="ignore") as f:
                    out += shlex.split(f.read())
                continue
            except OSError:
                pass
        out.append(a)
    return out


def code_model(args):
    """Sem -fPIC: large e nao-PIC (so executavel). Com -fPIC/-fPIE ou SODOS_PIC=1:
    PIC no modelo small, que tambem entra em .so; o TLS initial-exec dispensa
    __tls_get_addr e o ld o relaxa nos executaveis. O sodos-cxxlib64 faz o mesmo."""
    model = ["-mcmodel=large", "-fno-pic", "-fno-pie"]
    if os.environ.get("SODOS_PIC"):
        model = ["-fPIC", "-ftls-model=initial-exec"]
    for a in args:
        if a in ("-fPIC", "-fpic", "-fPIE", "-fpie"):
            model = ["-ftls-model=initial-exec"]
        elif a in ("-fno-pic", "-fno-PIC", "-fno-pie", "-fno-PIE"):
            model = ["-mcmodel=large", "-fno-pic", "-fno-pie"]
    return model


def c64_cmd(args):
    return (["gcc", "-m64", "-ffreestanding"] + code_model(args) +
            ["-fno-stack-protector", "-mno-red-zone",
             "-nostdinc", "-isystem", SODOS_INCLUDE,
             "-isystem", os.path.join(C, "include"),
             "-isystem", GCC_INCLUDE,
             "-D__SODOS__"])


def compile_one(kind, src, obj, flags):
    if is64():
        if kind == "cxx":
            return run([CXX64, "-c", src, "-o", obj] + flags)
        return run(c64_cmd(flags) + ["-c", src, "-o", obj] + flags)
    cc = CXX if kind == "cxx" else CC
    cmd = [cc, "-c", src, "-o", obj] + flags
    return run(cmd)


def kernel_stubs(tmp):
    """Um thunk e um slot para cada export do kernel (preenchidos pelo loader)."""
    asm = [".text"]
    data = [".data", ".balign 8"]
    with open(KERNEL, encoding="latin-1") as f:
        for line in f:
            name = line.strip()
            if not name or name.startswith(("LIBRARY", "EXPORTS", ";")):
                continue
            name = name.split()[0]
            asm += [".globl %s" % name, ".type %s,@function" % name, "%s:" % name,
                    "    jmp *__sodos_imp_%s(%%rip)" % name]
            data += [".globl __sodos_imp_%s" % name, "__sodos_imp_%s:" % name, "    .quad 0"]
    src = os.path.join(tmp, "kstubs.s")
    obj = os.path.join(tmp, "kstubs.o")
    with open(src, "w") as f:
        f.write("\n".join(asm + data + ['.section .note.GNU-stack,"",@progbits', ""]))
    if run(["gcc", "-c", src, "-o", obj]) != 0:
        return None
    return obj


# ---- Link dinamico: bibliotecas .so e executaveis ELF ----------------------
# O kernel carrega ELF direto (kernel/elf.c): -shared gera uma .so de verdade e o
# executavel sai como PIE ligado a libc.so.6, sem passar pelo SOX2. As
# bibliotecas do sistema ficam em libs/dso/build/sysroot/lib (make -C libs/dso).

# "dynamic": executavel ligado a libc.so.6, como em qualquer Unix. "static": tudo
# dentro do .EXE (SOX2). Vale quando nem -static nem SODOS_STATIC/SODOS_DYNAMIC
# decidem.
DEFAULT_LINK = "static"

# Opcoes de -Wl que nao fazem sentido aqui: o interpretador e o kernel, os
# grupos ja envolvem tudo e o resto e de outros linkers (lld/gold).
LD_DROP = ("--dynamic-linker", "-dynamic-linker", "-I", "--sysroot", "--hash-style",
           "--start-group", "--end-group", "-(", "-)", "--icf", "--gdb-index", "--threads",
           "--thinlto", "--lto", "-plugin", "--plugin", "--no-rosegment", "--pack-dyn-relocs",
           "--fatal-warnings", "--color-diagnostics", "--no-color-diagnostics")
# Opcoes de ld cujo valor vem no token seguinte (-Wl,-soname,libx.so).
LD_PAIR = ("-soname", "--soname", "-h", "-rpath", "--rpath", "-rpath-link", "--rpath-link",
           "-z", "-e", "--entry", "-u", "--undefined", "-T", "--script", "--version-script",
           "--dynamic-list", "-Map", "--defsym", "-L", "-l", "--exclude-libs", "-init", "-fini",
           "--retain-symbols-file", "--export-dynamic-symbol", "--require-defined",
           "--dynamic-linker", "-dynamic-linker", "-I", "--sysroot", "--hash-style",
           "-plugin", "--plugin", "--wrap", "-y", "--trace-symbol", "-m", "-b", "--format",
           "--build-id-dir", "-f", "--auxiliary", "-F", "--filter", "-G", "--gpsize")


def dynamic_runtime():
    return os.path.exists(os.path.join(SYSROOT_LIB, "libc.so.6"))


def link_mode(args):
    """Como ligar: "static" (SOX2 com a libc dentro) ou "dynamic" (ELF + libc.so.6)."""
    if "-static" in args or os.environ.get("SODOS_STATIC", "0") not in ("0", ""):
        return "static"
    mode = DEFAULT_LINK
    env = os.environ.get("SODOS_DYNAMIC")
    if env is not None:
        mode = "static" if env in ("0", "") else "dynamic"
    if mode == "dynamic" and not dynamic_runtime():
        return "static"
    return mode


def find_lib(name, dirs):
    for d in dirs:
        for suffix in (".so", ".a"):
            if os.path.exists(os.path.join(d, "lib%s%s" % (name, suffix))):
                return True
    return False


def ld_tokens(tokens):
    """Traduz as opcoes de um -Wl,a,b,c para o ld, descartando as de LD_DROP."""
    out = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        key = t.split("=", 1)[0]
        takes_next = t in LD_PAIR and "=" not in t and i + 1 < len(tokens)
        if key in LD_DROP or key.startswith(("--icf", "--lto", "--thinlto", "-plugin-opt")):
            i += 2 if takes_next else 1
            continue
        out.append(t)
        if takes_next:
            out.append(tokens[i + 1])
            i += 1
        i += 1
    return out


def gnu_link_dyn(args, out, kind, local_objs, shared):
    """Link ELF com o GNU ld, na ordem da linha de comando (como o gcc faz)."""
    nostdlib = "-nostdlib" in args
    startfiles = not (nostdlib or "-nostartfiles" in args)
    defaultlibs = not (nostdlib or "-nodefaultlibs" in args)
    # -nostdlib: nunca as bibliotecas do Linux do host (/usr/lib), so -L e o sysroot.
    cmd = ["ld", "-nostdlib", "--hash-style=both", "-z", "max-page-size=0x1000",
           "-z", "noexecstack", "-z", "notext", "-z", "norelro", "--no-eh-frame-hdr"]
    if not os.environ.get("SODOS_KEEP_DEBUG"):
        cmd.append("--strip-debug")
    if shared:
        cmd.append("-shared")
    else:
        # Sem PT_INTERP: quem carrega as bibliotecas e o kernel.
        cmd += ["-pie", "--no-dynamic-linker", "-e", "_start", "-z", "stack-size=0x1000000"]
    cmd += shlex.split(os.environ.get("SODOS_LDFLAGS", ""))
    cmd += ["-o", out]
    if startfiles:
        if not shared:
            cmd.append(os.path.join(SYSROOT_LIB, "crt1.o"))
        cmd.append(os.path.join(SYSROOT_LIB, "crtdso.o"))
    libdirs = []
    inputs = list(local_objs)
    i = 0
    while i < len(args):
        a = args[i]
        nxt = args[i + 1] if i + 1 < len(args) else None
        if a in ("-o", "-I", "-D", "-U", "-isystem", "-idirafter", "-iquote", "-include",
                 "-imacros", "-MF", "-MT", "-MQ", "-x", "-B", "--param", "-arch", "-target"):
            i += 2
            continue
        if a == "-L" and nxt is not None:
            libdirs.append(nxt)
            inputs += ["-L", nxt]
            i += 2
            continue
        if a.startswith("-L"):
            libdirs.append(a[2:])
            inputs.append(a)
            i += 1
            continue
        if a == "-l" and nxt is not None:
            a = "-l" + nxt
            i += 1
        if a.startswith("-l"):
            # Bibliotecas que o SODOS nao tem (-lstdc++, -lgcc_s...) ja estao na
            # libc/libc++: somem em silencio, como no link estatico.
            if a[2:].startswith(":") or find_lib(a[2:], libdirs + [SYSROOT_LIB]):
                inputs.append(a)
            i += 1
            continue
        if a.startswith("-Wl,"):
            inputs += ld_tokens([t for t in a[4:].split(",") if t])
            i += 1
            continue
        if a == "-Xlinker" and nxt is not None:
            inputs += ld_tokens([nxt])
            i += 2
            continue
        if a in ("-z", "-u", "-T", "-e") and nxt is not None:
            inputs += [a, nxt]
            i += 2
            continue
        if a == "-rdynamic":
            inputs.append("--export-dynamic")
        elif a == "-s":
            inputs.append("--strip-all")
        elif a.startswith("-"):
            pass  # opcao de compilacao ou do driver
        elif a.lower().endswith((".c", ".cc", ".cpp", ".cxx", ".s", ".sx")):
            pass  # ja compilado para local_objs
        elif os.path.exists(a):
            inputs.append(a)
        i += 1
    cmd += ["--start-group"] + inputs + ["--end-group"]
    # As DT_NEEDED das bibliotecas ligadas (libm.so.6 de uma libfoo.so) tambem
    # saem do sysroot e dos -L, nao do host.
    for d in libdirs + [SYSROOT_LIB]:
        cmd += ["-rpath-link", d]
    if defaultlibs:
        cmd += ["-L", SYSROOT_LIB]
        if kind == "cxx" and os.path.exists(os.path.join(SYSROOT_LIB, "libc++.so.1")):
            cmd.append("-lc++")
        cmd += ["-lc", "-lkernel"]
        # libgcc do gcc do host (x86-64, PIC): __divti3/__udivti3 de __int128 etc.
        libgcc = subprocess.check_output(["gcc", "-print-libgcc-file-name"], text=True).strip()
        if os.path.exists(libgcc):
            cmd.append(libgcc)
    r = run(cmd)
    if r == 0:
        os.chmod(out, 0o755)
    return r


def gnu_link(out, kind, local_objs, objs, rlibs, tmp, shared):
    """-shared vira um objeto relocavel; o programa final e PIE e depois SOX2."""
    if shared:
        cmd = ["ld", "-r", "--allow-multiple-definition", "-o", out] + local_objs + objs
        if rlibs:
            cmd += ["--whole-archive"] + rlibs + ["--no-whole-archive"]
        return run(cmd)
    stubs = kernel_stubs(tmp)
    if not stubs:
        return 1
    libs = []
    if kind == "cxx":
        # Sem INIT64: ele redefine __init_array_start, que aqui vem do ld.
        libs += [CXXRT64, OVR64]
        libs += glob.glob(os.path.join(ROOT, "build", "libcxx64", "obj", "*.o"))
    libs += glob.glob(os.path.join(PORT64, "*.o"))
    if os.path.exists(POSIX64):
        libs.append(POSIX64)
    # Com --allow-multiple-definition vence a primeira definicao, e o glob segue
    # a ordem do diretorio: x64_misc.o (__sodos_ctors vazio, para a LIBCX) ja
    # passou na frente do x64_ctors64.o e nenhum construtor rodava (sem TLS, o
    # QEMU caia no primeiro thread_local). O ctors64 vai sempre primeiro.
    x64 = sorted(glob.glob(os.path.join(C, "build", "x64_*.o")))
    x64.sort(key=lambda f: os.path.basename(f) != "x64_ctors64.o")
    libs += x64
    # libgcc do gcc do host (x86-64, PIC): __divti3/__udivti3 de __int128 etc.
    libgcc = subprocess.check_output(["gcc", "-print-libgcc-file-name"], text=True).strip()
    if os.path.exists(libgcc):
        libs.append(libgcc)
    elf = out + ".elf"
    cmd = ["ld", "-pie", "--no-dynamic-linker", "-z", "notext", "-z", "norelro",
           "-z", "max-page-size=0x1000", "-z", "noexecstack", "--no-eh-frame-hdr",
           "--gc-sections", "--strip-debug", "--allow-multiple-definition", "-e", "_start",
           "--defsym=__sodos_stack_size=0x1000000", "-o", elf, CRT64, stubs]
    cmd += local_objs + objs + ["--start-group"] + rlibs + libs + ["--end-group"]
    r = run(cmd)
    if r != 0:
        return r
    r = run([sys.executable, os.path.join(HERE, "elf2sox2.py"), elf, out])
    if r == 0:
        os.chmod(out, 0o755)
        if not os.environ.get("SODOS_KEEP_ELF"):
            os.unlink(elf)
    return r


def main():
    args = expand_response_files(sys.argv[1:])
    # O meson pergunta "-m64 --version": flags -m de ABI antes da consulta
    # nao mudam a resposta.
    info = [a for a in args if a not in ("-m64", "-mx32", "-m32")]
    if info:
        r = print_info(info[0])
        if r is not None:
            return r
        # O configure pergunta a versao do linker por -Wl,--version; nao ha
        # ld ELF no alvo, entao respondemos como um ld GNU (o link real e SOX).
        if any(a in ("-Wl,--version", "-Wl,-v", "-Wl,-V") for a in args):
            print("GNU ld (SODOS link) 1.0")
            return 0

    compile_only = any(a in ("-c", "-E", "-S", "-M", "-MM") for a in args)
    if compile_only:
        if is64():
            # Como no x32: so o -E do configure deixa de ver __linux__.
            undef = ["-U__linux__", "-Ulinux"] if "-E" in args else []
            if is_cxx():
                return run([CXX64] + undef + args)
            return run(c64_cmd(args) + undef + args)
        cc = CXX if is_cxx() else CC
        # O configure descobre o kernel preprocessando um conftest; ai o alvo
        # precisa parecer SODOS e nao Linux. Compilar/ligar mantem __linux__
        # porque a libc++ usa isso para detectar pthreads.
        undef = ["-U__linux__", "-Ulinux"] if "-E" in args else []
        return run([cc] + undef + args)

    out = None
    srcs = []
    objs = []
    rlibs = []
    flags = []
    libdirs = []
    libnames = []
    shared = False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o":
            if i + 1 >= len(args):
                sys.stderr.write("sodos-gcc: -o sem argumento\n")
                return 1
            out = args[i + 1]
            i += 2
            continue
        if a.startswith("-o") and len(a) > 2:
            out = a[2:]
            i += 1
            continue
        if a in ("-Wl,", "-Xlinker"):
            i += 2
            continue
        if a.startswith("-Wl,"):
            i += 1
            continue
        if a in ("-shared", "-static", "-rdynamic", "-s", "-nostdlib",
                 "-nodefaultlibs", "-nostartfiles", "-pthread"):
            if a == "-shared":
                shared = True
            i += 1
            continue
        if a == "-L" and i + 1 < len(args):
            libdirs.append(args[i + 1])
            i += 2
            continue
        if a in ("-z", "-T", "-u", "-rpath", "-rpath-link", "-l"):
            i += 2
            continue
        if a.startswith("-L"):
            libdirs.append(a[2:])
            i += 1
            continue
        if a.startswith("-l"):
            libnames.append(a[2:])
            i += 1
            continue
        # Opcoes de compilacao com o valor no argumento seguinte: o valor nao e
        # arquivo de entrada (o meson liga com "-idirafter DIR" nos flags).
        if a in ("-idirafter", "-isystem", "-iquote", "-include", "-imacros", "-I",
                 "-D", "-U", "-MF", "-MT", "-MQ", "-x") and i + 1 < len(args):
            flags += [a, args[i + 1]]
            i += 2
            continue
        if a.startswith("-B") or a.startswith("--") or a in ("-O1", "-O2", "-O3", "-Os"):
            i += 1
            continue
        if a.startswith("-"):
            flags.append(a)
            i += 1
            continue
        low = a.lower()
        if low.endswith((".c", ".cc", ".cpp", ".cxx", ".s", ".sx")):
            srcs.append(a)
        elif low.endswith((".o", ".obj")):
            objs.append(a)
        elif low.endswith((".rlib", ".a")):
            rlibs.append(a)
        else:
            # .a/.lib e outros: o link do SODOS nao le .a; tenta objeto.
            if os.path.exists(a):
                objs.append(a)
        i += 1

    # -lfoo vira libfoo.a quando existe em algum -L (bibliotecas portadas para
    # o sysroot, ex.: glib/zlib do QEMU). As demais (-lm, -lpthread, -lutil...)
    # ja estao na libc/libsodos e continuam ignoradas.
    for name in libnames:
        for d in libdirs:
            p = os.path.join(d, "lib%s.a" % name)
            if os.path.exists(p):
                if p not in rlibs:
                    rlibs.append(p)
                break

    linker = LINK64 if is64() else LINK
    crt = CRT64 if is64() else CRT
    if not os.access(linker, os.X_OK):
        sys.stderr.write("sodos-gcc: %s nao existe; rode make -C c_compiler\n" % linker)
        return 1
    if not os.path.exists(crt):
        sys.stderr.write("sodos-gcc: %s nao existe; rode make -C c_compiler\n" % crt)
        return 1
    if is64() and is_cxx():
        for need in (CXXRT64, INIT64, OVR64):
            if not os.path.exists(need):
                sys.stderr.write("sodos-g++64: %s nao existe; rode make libcxx64\n" % need)
                return 1

    kind = "cxx" if is_cxx() else "c"
    tmp = tempfile.mkdtemp(prefix="sodos-link-")
    try:
        local_objs = []
        for n, src in enumerate(srcs):
            obj = os.path.join(tmp, "s%d.o" % n)
            if compile_one(kind, src, obj, flags) != 0:
                return 1
            local_objs.append(obj)

        if out is None:
            out = "a.out"

        if is64() and os.environ.get("SODOS_LINKER") != "link64":
            if link_mode(args) == "dynamic":
                return gnu_link_dyn(args, out, kind, local_objs, shared)
            return gnu_link(out, kind, local_objs, objs, rlibs, tmp, shared)

        cmd = [linker]
        if is64():
            cmd += ["/out:" + out, crt]
            cmd += local_objs + objs
            if kind == "cxx":
                cmd += [CXXRT64, INIT64, OVR64]
                cmd += glob.glob(os.path.join(ROOT, "build", "libcxx64", "obj", "*.o"))
            # pthreads e ABI Linux: tambem em C, para o configure achar pthread_create.
            cmd += glob.glob(os.path.join(PORT64, "*.o"))
            if os.path.exists(POSIX64):
                cmd.append(POSIX64)
            if rlibs:
                combined = os.path.join(tmp, "rust_combined.o")
                ld = ["ld", "-r", "--force-group-allocation", "--allow-multiple-definition"] + objs
                ld += ["--whole-archive"] + rlibs + ["--no-whole-archive", "-o", combined]
                if run(ld) != 0:
                    return 1
                cmd = [linker, "/out:" + out, crt, combined]
                if kind == "cxx":
                    cmd += [CXXRT64, INIT64, OVR64]
                    cmd += glob.glob(os.path.join(ROOT, "build", "libcxx64", "obj", "*.o"))
                # O std do Rust usa a ABI Linux (sodos_linux_abi.c), mesmo em C.
                cmd += glob.glob(os.path.join(PORT64, "*.o"))
                if os.path.exists(POSIX64):
                    cmd.append(POSIX64)
            cmd += glob.glob(os.path.join(C, "build", "x64_*.o"))
            if os.path.exists(KERNEL):
                cmd.append(KERNEL)
            if rlibs:
                # A libc (malloc/new) deve vencer os simbolos duplicados da
                # Rust std dentro do combinado; o link64 fica com a 1a def.
                if combined in cmd:
                    cmd.remove(combined)
                    cmd.append(combined)
        elif shared:
            cmd += ["/dll", "/exportall"]
        else:
            cmd += [crt]
            cmd += ["/out:" + out]
            cmd += local_objs + objs
            for lib in (LIBCX, KERNEL, POSIXX, SODOS):
                if os.path.exists(lib):
                    cmd.append(lib)
            if kind == "cxx":
                rt = CXXRT if os.path.exists(CXXRT) else CXXRT2
                if os.path.exists(rt):
                    cmd.append(rt)
                if os.path.exists(LIBCXX):
                    cmd.append(LIBCXX)
            for extra in (FP, MATH):
                if os.path.exists(extra):
                    cmd.append(extra)
        if is64():
            r = run(cmd)
            if r == 0:
                try:
                    os.chmod(out, 0o755)
                except OSError:
                    pass
            return r
        r = run(cmd)
        if r == 0 and not shared:
            try:
                os.chmod(out, 0o755)
            except OSError:
                pass
        return r
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
