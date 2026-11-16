#!/usr/bin/env python3
"""llvm-config-sodos - responde o meson do Mesa pelo LLVM cruzado para o SODOS.

O llvm-config do build cruzado e um executavel do SODOS e nao roda no host;
este script le a pasta do build (LLVM_BUILD) e a fonte (LLVM_SRC) e devolve o
que o meson pergunta. Todas as bibliotecas sao estaticas (libLLVM*.a); a ordem
nao importa porque o build-mesa.sh liga tudo dentro de --start-group.
"""
import glob
import os
import sys

B = os.environ.get("LLVM_BUILD", os.path.expanduser("~/ffsrc/mesa-build/llvm"))
SRC = os.environ.get("LLVM_SRC", os.path.expanduser("~/ffsrc/llvm18/llvm"))
VERSION = "18.1.8"


def libs():
    return sorted(glob.glob(os.path.join(B, "lib", "libLLVM*.a")))


def components():
    names = set()
    for p in libs():
        n = os.path.basename(p)[len("libLLVM"):-2].lower()
        names.add(n)
    # Componentes "virtuais" do LLVMBuild.
    names |= {"all", "all-targets", "engine", "native", "nativecodegen", "x86"}
    return sorted(names)


def main(args):
    out = []
    cpp = ["-I" + os.path.join(SRC, "include"), "-I" + os.path.join(B, "include"),
           "-D__STDC_CONSTANT_MACROS", "-D__STDC_FORMAT_MACROS", "-D__STDC_LIMIT_MACROS"]
    for a in args:
        if a == "--version":
            out.append(VERSION)
        elif a in ("--link-static", "--link-shared"):
            continue
        elif a == "--shared-mode":
            out.append("static")
        elif a == "--has-rtti":
            out.append("NO")
        elif a == "--assertion-mode":
            out.append("OFF")
        elif a == "--build-mode":
            out.append("Release")
        elif a == "--targets-built":
            out.append("X86")
        elif a == "--components":
            out.append(" ".join(components()))
        elif a == "--libs":
            out.append(" ".join(libs()))
        elif a == "--libfiles":
            out.append(" ".join(libs()))
        elif a == "--libnames":
            out.append(" ".join(os.path.basename(p) for p in libs()))
        elif a == "--system-libs":
            out.append("")
        elif a == "--ldflags":
            out.append("-L" + os.path.join(B, "lib"))
        elif a == "--cppflags":
            out.append(" ".join(cpp))
        elif a == "--cflags":
            out.append(" ".join(cpp))
        elif a == "--cxxflags":
            out.append(" ".join(cpp + ["-std=c++17", "-fno-exceptions", "-fno-rtti"]))
        elif a == "--libdir":
            out.append(os.path.join(B, "lib"))
        elif a == "--includedir":
            out.append(os.path.join(SRC, "include"))
        elif a in ("--prefix", "--obj-root"):
            out.append(B)
        elif a == "--src-root":
            out.append(SRC)
        elif a == "--bindir":
            out.append(os.path.join(B, "bin"))
        elif a == "--host-target":
            out.append("x86_64-unknown-linux-gnu")
        elif a.startswith("-"):
            sys.stderr.write("llvm-config-sodos: opcao desconhecida %s\n" % a)
            return 1
        # Nomes de componentes (--libs core mcjit ...) sao ignorados.
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
