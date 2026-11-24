#!/usr/bin/python3
"""YTPLAY.PY - ajudante do PLAY.EXE, roda no Linux do WSL do SODOS.

O PLAY.EXE (no WIN) pede pela caixa do WSL:
  python3 /d/develop/play/YTPLAY.PY 'LINK' [LARGURA] [QPS] [QUALIDADE]

O yt-dlp acha e baixa o video do YouTube (so a imagem, sem som) e passa os
bytes pelo stdin do ffmpeg minimo (linux/ffmpeg), que converte para MJPEG:
uma sequencia de JPEG no stdout, que vai pela caixa ate o PLAY.EXE.
O estado vai no stderr, uma linha por vez, em CP850:
  PLAY titulo <texto>
  PLAY duracao <segundos>
  PLAY formato <id> <codec> <LxA>
  PLAY erro <mensagem>
  PLAY fim <codigo do yt-dlp> <codigo do ffmpeg>
SIGINT (o PLAY fechou) para os dois.
"""
import os
import shutil
import subprocess
import sys
import threading
import time

AQUI = os.path.dirname(os.path.abspath(__file__))
LOG = "/tmp/ytplay.log"          # diagnostico, na VM (some no boot dela)
T0 = time.time()


def log(*partes):
    try:
        with open(LOG, "a") as f:
            f.write("%s %6.1f [%d] %s\n" % (time.strftime("%H:%M:%S"), time.time() - T0,
                                            os.getpid(), " ".join(str(p) for p in partes)))
    except OSError:
        pass
LINUX = os.path.join(AQUI, "linux")
YTDLP = os.path.join(LINUX, "yt-dlp")
FFSRC = os.path.join(LINUX, "ffmpeg")
FFBIN = "/tmp/play-ffmpeg"      # o D: e FAT: sem bit de execucao

# Imagem so, ate 360p (a saida e menor que isso); H.264 primeiro, depois VP9.
# Apenas https: o ffmpeg minimo le MP4 e WebM pelo pipe, nao HLS.
FORMATO = ("bv*[vcodec^=avc1][height<=360][protocol^=https]/"
           "bv*[vcodec^=vp9][height<=360][protocol^=https]/"
           "bv*[vcodec^=avc1][height<=480][protocol^=https]/"
           "bv*[vcodec^=vp9][height<=480][protocol^=https]/"
           "b[height<=480][protocol^=https]")


def status(*partes):
    linha = "PLAY " + " ".join(str(p) for p in partes)
    linha = linha.replace("\r", " ").replace("\n", " ")[:400]
    try:
        sys.stderr.buffer.write(linha.encode("cp850", "replace") + b"\n")
        sys.stderr.buffer.flush()
    except (BrokenPipeError, OSError):
        pass


class Log:
    def debug(self, m):
        pass

    def info(self, m):
        pass

    def warning(self, m):
        log("aviso", m)

    def error(self, m):
        m = str(m)
        log("erro", m)
        if m.startswith("ERROR: "):
            m = m[7:]
        status("erro", m)


def prepara_ffmpeg():
    """Copia o ffmpeg do D: para o /tmp da VM (uma vez por boot da VM)."""
    tam = os.stat(FFSRC).st_size
    try:
        if os.stat(FFBIN).st_size == tam:
            return
    except FileNotFoundError:
        pass
    tmp = FFBIN + ".tmp"
    shutil.copyfile(FFSRC, tmp)
    os.chmod(tmp, 0o755)
    os.replace(tmp, FFBIN)


def repassa_erros(stream):
    for linha in iter(stream.readline, b""):
        texto = linha.decode("utf-8", "replace").strip()
        if texto:
            status("erro", "ffmpeg: " + texto)


def numero(i, padrao, minimo, maximo):
    try:
        v = int(sys.argv[i])
    except (IndexError, ValueError):
        return padrao
    return max(minimo, min(maximo, v))


def main():
    if len(sys.argv) < 2:
        status("erro", "uso: YTPLAY.PY LINK [LARGURA] [QPS] [QUALIDADE]")
        return 2
    url = sys.argv[1]
    largura = numero(2, 480, 160, 800) // 2 * 2
    qps = numero(3, 15, 1, 30)
    qualidade = numero(4, 7, 2, 31)

    try:
        prepara_ffmpeg()
    except OSError as e:
        status("erro", "ffmpeg: %s" % e)
        return 3
    sys.path.insert(0, YTDLP)
    try:
        import yt_dlp
    except Exception as e:  # noqa: BLE001 - qualquer falha do zipapp
        status("erro", "yt-dlp: %s" % e)
        return 3

    # fd 1 e o pipe para o SODOS: o ffmpeg escreve nele; o yt-dlp escreve o
    # video no fd 1, que passa a ser a entrada do ffmpeg.
    saida = os.dup(1)
    ff = subprocess.Popen(
        [FFBIN, "-hide_banner", "-loglevel", "error", "-i", "pipe:0", "-an",
         "-vf", "fps=%d,scale=%d:-2" % (qps, largura),
         "-q:v", str(qualidade), "-f", "mjpeg", "pipe:1"],
        stdin=subprocess.PIPE, stdout=saida, stderr=subprocess.PIPE)
    os.close(saida)
    os.dup2(ff.stdin.fileno(), 1)
    ff.stdin.close()
    threading.Thread(target=repassa_erros, args=(ff.stderr,), daemon=True).start()

    meta = {"feito": False, "t": 0.0}
    log("inicio", url, largura, qps, qualidade)

    def gancho(d):
        agora = time.time()
        if d.get("status") != "downloading" or agora - meta["t"] >= 30:
            meta["t"] = agora
            log("download", d.get("status"), d.get("downloaded_bytes"), "de",
                d.get("total_bytes") or d.get("total_bytes_estimate"))
        if meta["feito"]:
            return
        meta["feito"] = True
        info = d.get("info_dict") or {}
        status("titulo", info.get("title") or "?")
        status("duracao", int(info.get("duration") or 0))
        status("formato", info.get("format_id") or "?", info.get("vcodec") or "?",
               "%sx%s" % (info.get("width") or "?", info.get("height") or "?"))

    opts = {
        "format": FORMATO,
        "outtmpl": "-",
        "quiet": True,
        "no_warnings": True,
        "noprogress": True,
        "noplaylist": True,
        "cachedir": False,
        "logger": Log(),
        "progress_hooks": [gancho],
    }
    rc = 1
    try:
        with yt_dlp.YoutubeDL(opts) as ydl:
            rc = ydl.download([url])
    except KeyboardInterrupt:
        log("SIGINT (o PLAY pediu para parar)")
        rc = 130
    except Exception as e:  # noqa: BLE001 - mostra o erro no PLAY
        log("excecao", repr(e))
        if not str(e).startswith("ERROR"):
            status("erro", e)
        rc = 1
    finally:
        try:
            sys.stdout.flush()
        except (BrokenPipeError, OSError):
            pass
        os.close(1)          # fim da entrada: o ffmpeg termina o que tem
    try:
        ffrc = ff.wait(timeout=60)
    except (KeyboardInterrupt, subprocess.TimeoutExpired):
        ff.kill()
        ffrc = ff.wait()
    log("fim yt-dlp", rc, "ffmpeg", ffrc)
    status("fim", rc, ffrc)
    return 0 if rc == 0 and ffrc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
