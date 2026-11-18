#!/usr/bin/env python3
"""stage-flutter-web.py - monta a arvore para compilar um app Flutter web com o
dart2js de outro sistema (o SODOS), sem o flutter_tools.

    python3 -I stage-flutter-web.py APP_DIR SAIDA RAIZ [--sdk DART_SDK] [--flutter FLUTTER]

APP_DIR  projeto Flutter ja construido uma vez no host (flutter build web): o
         .dart_tool/flutter_build/<hash>/ tem o main.dart gerado e o app.dill.deps,
         que diz quais fontes entram.
SAIDA    pasta local que vira RAIZ no outro sistema (ex.: dart/build/flfs/FL).
RAIZ     o caminho POSIX dela la dentro (ex.: /d/FL; o port do Dart usa /d/... ).

Gera em SAIDA:
  p/<pacote>/lib/...       so os fontes listados no app.dill.deps
  app/lib/...              o app (pacote raiz)
  build/main.dart          bootstrap do flutter build web (+ web_plugin_registrant.dart)
  build/package_config.json  com rootUri file://RAIZ/...
  sdk/libraries.json       o do dart-sdk do Flutter (o dart compile js passa este)
  sdk/kernel/dart2js_platform.dill  o do flutter_web_sdk (tem dart:ui)
  d2j-cfe.args, d2j-js.args  argumentos das duas fases (uma linha por argumento)
"""
import json
import os
import re
import shutil
import sys
from urllib.parse import unquote, urlparse


def file_uri_to_path(u):
    p = urlparse(u)
    path = unquote(p.path)
    # file:///C:/x -> C:/x no Windows; no WSL vira /mnt/c/x
    m = re.match(r"^/([A-Za-z]):/(.*)$", path)
    if m:
        drive, rest = m.group(1).lower(), m.group(2)
        if os.name == "nt":
            return f"{drive.upper()}:/{rest}"
        return f"/mnt/{drive}/{rest}"
    return path


def main():
    args = sys.argv[1:]
    opts = {}
    pos = []
    i = 0
    while i < len(args):
        if args[i] in ("--sdk", "--flutter"):
            opts[args[i]] = args[i + 1]
            i += 2
        else:
            pos.append(args[i])
            i += 1
    if len(pos) != 3:
        sys.exit(__doc__)
    app, out, root = pos
    root = root.rstrip("/")
    flutter = opts.get("--flutter")
    if not flutter:
        cfg0 = json.load(open(os.path.join(app, ".dart_tool", "package_config.json")))
        fl = next(p for p in cfg0["packages"] if p["name"] == "flutter")
        flutter = os.path.dirname(os.path.dirname(file_uri_to_path(fl["rootUri"])))
    dart_sdk = opts.get("--sdk") or os.path.join(flutter, "bin", "cache", "dart-sdk")
    web_sdk = os.path.join(flutter, "bin", "cache", "flutter_web_sdk")

    fb = os.path.join(app, ".dart_tool", "flutter_build")
    builds = [os.path.join(fb, d) for d in os.listdir(fb) if os.path.isfile(os.path.join(fb, d, "app.dill.deps"))]
    if not builds:
        sys.exit("sem .dart_tool/flutter_build/*/app.dill.deps: rode flutter build web no host antes")
    build = max(builds, key=lambda d: os.path.getmtime(os.path.join(d, "app.dill.deps")))

    cfg_path = os.path.join(app, ".dart_tool", "package_config.json")
    cfg = json.load(open(cfg_path))
    cfg_dir = os.path.dirname(os.path.abspath(cfg_path))
    pkgs = {}
    for p in cfg["packages"]:
        r = p["rootUri"]
        if r.startswith("file:"):
            d = file_uri_to_path(r)
        else:
            d = os.path.normpath(os.path.join(cfg_dir, r))
        pkgs[p["name"]] = (os.path.normcase(os.path.abspath(d)), p)
    app_name = cfg.get("name") or next(n for n, (d, _) in pkgs.items() if os.path.samefile(d, app))

    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)

    deps = [l.strip() for l in open(os.path.join(build, "app.dill.deps")) if l.strip()]
    used = set()
    copied = 0
    for u in deps:
        if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]+:", u) and not u.startswith("file:"):
            continue  # org-dartlang-sdk:, etc.: vem do dill de plataforma
        src = os.path.abspath(file_uri_to_path(u)) if u.startswith("file:") else os.path.abspath(u)
        nsrc = os.path.normcase(src)
        if nsrc.startswith(os.path.normcase(os.path.abspath(build))):
            continue  # main.dart / registrant: vao para build/
        best = None
        for name, (d, _) in pkgs.items():
            if nsrc.startswith(d + os.sep) and (best is None or len(d) > len(pkgs[best][0])):
                best = name
        if best is None:
            continue  # dill de plataforma, etc.
        rel = os.path.relpath(src, pkgs[best][0])
        dst_root = "app" if best == app_name else os.path.join("p", best)
        dst = os.path.join(out, dst_root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        used.add(best)
        copied += 1

    new_pkgs = []
    for name, (d, p) in pkgs.items():
        if name not in used and name != app_name:
            continue
        sub = "app" if name == app_name else f"p/{name}"
        q = dict(p)
        q["rootUri"] = f"file://{root}/{sub}/"
        new_pkgs.append(q)
    new_cfg = {k: v for k, v in cfg.items() if k != "packages"}
    new_cfg["packages"] = new_pkgs
    os.makedirs(os.path.join(out, "build"))
    json.dump(new_cfg, open(os.path.join(out, "build", "package_config.json"), "w"), indent=1)
    for f in ("main.dart", "web_plugin_registrant.dart"):
        if os.path.exists(os.path.join(build, f)):
            shutil.copyfile(os.path.join(build, f), os.path.join(out, "build", f))

    os.makedirs(os.path.join(out, "sdk", "kernel"))
    shutil.copyfile(os.path.join(dart_sdk, "lib", "libraries.json"), os.path.join(out, "sdk", "libraries.json"))
    shutil.copyfile(os.path.join(web_sdk, "kernel", "dart2js_platform.dill"),
                    os.path.join(out, "sdk", "kernel", "dart2js_platform.dill"))

    # Os -D do flutter build web (release, CanvasKit).
    ver = json.load(open(os.path.join(flutter, "bin", "cache", "flutter.version.json")))
    defines = [
        "-Ddart.vm.product=true",
        "-DFLUTTER_BUILD_NAME=1.0.0",
        "-DFLUTTER_BUILD_NUMBER=1",
        f"-DFLUTTER_VERSION={ver['flutterVersion']}",
        f"-DFLUTTER_CHANNEL={ver['channel']}",
        f"-DFLUTTER_GIT_URL={ver['repositoryUrl']}",
        f"-DFLUTTER_FRAMEWORK_REVISION={ver['frameworkRevision'][:10]}",
        f"-DFLUTTER_ENGINE_REVISION={ver['engineRevision'][:10]}",
        f"-DFLUTTER_DART_VERSION={ver['dartSdkVersion']}",
        "-DFLUTTER_WEB_USE_SKIA=true",
        "-DFLUTTER_WEB_USE_SKWASM=false",
        f"-DFLUTTER_WEB_CANVASKIT_URL=https://www.gstatic.com/flutter-canvaskit/{ver['engineRevision']}/",
    ]
    common = [
        f"--libraries-spec={root}/sdk/libraries.json",
        "--cfe-invocation-modes=compile",
        f"--platform-binaries={root}/sdk/kernel",
        "--invoker=flutter_tool",
    ] + defines + [
        "--enable-experiment=record-use",
        "--native-null-assertions",
        "--no-source-maps",
        "-O4",
        "--minify",
    ]
    cfe = common + [
        f"-o{root}/build/app.dill",
        f"--packages={root}/build/package_config.json",
        "--cfe-only",
        f"{root}/build/main.dart",
    ]
    js = common + [f"-o{root}/build/main.dart.js", f"{root}/build/app.dill"]
    open(os.path.join(out, "d2j-cfe.args"), "w").write("\n".join(cfe) + "\n")
    open(os.path.join(out, "d2j-js.args"), "w").write("\n".join(js) + "\n")
    print(f"{copied} fontes de {len(used)} pacotes; build={os.path.basename(build)}; raiz {root}")


if __name__ == "__main__":
    main()
