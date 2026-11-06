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
        return subprocess.check_output([exe, "--version"], text=True).splitlines()[0]
    except Exception:
        return "gcc (SODOS) 13.3.0"


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


def compile_one(kind, src, obj, flags):
    if is64():
        if kind == "cxx":
            return run([CXX64, "-c", src, "-o", obj] + flags)
        cmd = ["gcc", "-m64", "-ffreestanding", "-fno-pic", "-fno-pie",
               "-fno-stack-protector", "-mno-red-zone", "-mcmodel=large",
               "-nostdinc", "-isystem", SODOS_INCLUDE,
               "-isystem", os.path.join(C, "include"),
               "-isystem", GCC_INCLUDE,
               "-D__SODOS__", "-c", src, "-o", obj] + flags
        return run(cmd)
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
    libs += glob.glob(os.path.join(C, "build", "x64_*.o"))
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
    if args:
        r = print_info(args[0])
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
            return run(["gcc", "-m64", "-ffreestanding", "-fno-pic", "-fno-pie",
                        "-fno-stack-protector", "-mno-red-zone", "-mcmodel=large",
                        "-nostdinc", "-isystem", SODOS_INCLUDE,
                        "-isystem", os.path.join(C, "include"),
                        "-isystem", GCC_INCLUDE,
                        "-D__SODOS__"] + undef + args)
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
        if a in ("-L", "-z", "-T", "-u", "-rpath", "-rpath-link"):
            i += 2
            continue
        if a.startswith("-l") or a.startswith("-L"):
            i += 1
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
