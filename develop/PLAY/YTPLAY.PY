#!/usr/bin/python3
"""YTPLAY.PY - ajudante do PLAY.EXE, roda no Linux do WSL do SODOS.

O PLAY.EXE (no WIN) pede pela caixa do WSL:
  python3 /d/develop/play/YTPLAY.PY 'LINK' [LARGURA] [QPS] [QUALIDADE] [HZ] [CANAIS]

O yt-dlp acha o video no YouTube. Este script baixa os formatos escolhidos
(imagem e, com HZ, o som) em pedacos com Range e passa os bytes ao ffmpeg
minimo (linux/ffmpeg), que converte a imagem para MJPEG (uma sequencia de
JPEG) e o som para PCM de 16 bits (HZ, CANAIS).

Sem HZ (ou HZ 0): so a imagem, e o stdout leva o MJPEG cru, como antes.
Com HZ: o stdout leva pacotes, cada um com 4 bytes de cabecalho
  tipo (1 byte: H, V ou A) + tamanho (3 bytes, little-endian) + dados
  H  o primeiro pacote: texto "SOM hz canais" (hz 0 = o video nao tem som)
  V  pedaco do MJPEG (os JPEG atravessam pacotes)
  A  pedaco do PCM s16le intercalado
O estado vai no stderr, uma linha por vez, em CP850:
  PLAY titulo <texto>
  PLAY duracao <segundos>
  PLAY formato <id> <codec> <LxA>
  PLAY som <id> <codec> | PLAY som nenhum
  PLAY erro <mensagem>
  PLAY fim <codigo do download> <codigo do ffmpeg>
SIGINT (o PLAY fechou) para tudo.
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
# Apenas https: o download daqui le MP4 e WebM direto, nao HLS.
VIDEO = ("bv*[vcodec^=avc1][height<=360][protocol^=https]/"
         "bv*[vcodec^=vp9][height<=360][protocol^=https]/"
         "bv*[vcodec^=avc1][height<=480][protocol^=https]/"
         "bv*[vcodec^=vp9][height<=480][protocol^=https]")
JUNTO = "b[height<=480][protocol^=https]"     # imagem e som no mesmo arquivo
# Som: AAC (m4a) primeiro, depois Opus (webm); os dois o ffmpeg decodifica.
SOM = ("ba[acodec^=mp4a][protocol^=https]/"
       "ba[acodec^=opus][protocol^=https]")
FORMATO = VIDEO + "/" + JUNTO
FORMATO_SOM = "(%s)+(%s)/%s/%s" % (VIDEO, SOM, JUNTO, VIDEO)

PEDACO = 10 * 1024 * 1024        # Range por pedido (o yt-dlp usa o mesmo)
TENTATIVAS = 8                   # quedas seguidas antes de desistir


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
    """Copia o ffmpeg do D: para o /tmp da VM (no boot da VM e quando muda:
    tamanho diferente ou o do D: mais novo que a copia)."""
    src = os.stat(FFSRC)
    try:
        dst = os.stat(FFBIN)
        if dst.st_size == src.st_size and dst.st_mtime >= src.st_mtime:
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


def escreve(fd, dados):
    """os.write ate o fim (o pipe cheio segura: e o ritmo do PLAY)."""
    vista = memoryview(dados)
    while vista:
        n = os.write(fd, vista)
        vista = vista[n:]


def baixa(ydl, f, fd, nome, estado):
    """Baixa o formato f em pedacos (Range) e escreve no fd (pipe do ffmpeg).

    Uma queda (rede, o YouTube fechando a conexao parada numa pausa longa)
    continua do byte onde parou. Fecha o fd no fim: o ffmpeg ve o fim."""
    from yt_dlp.networking import Request
    from yt_dlp.networking.exceptions import HTTPError, RequestError
    url = f["url"]
    cab = dict(f.get("http_headers") or {})
    total = f.get("filesize") or 0
    pedaco = (f.get("downloader_options") or {}).get("http_chunk_size") or PEDACO
    pos, quedas, t_log = 0, 0, 0.0
    rc = 1
    try:
        while not estado["parar"] and (not total or pos < total):
            fim = pos + pedaco - 1
            if total:
                fim = min(fim, total - 1)
            h = dict(cab)
            h["Range"] = "bytes=%d-%d" % (pos, fim)
            antes = pos
            try:
                r = ydl.urlopen(Request(url, headers=h))
                if not total:
                    cr = r.headers.get("Content-Range") or ""
                    if "/" in cr and cr.rsplit("/", 1)[1].isdigit():
                        total = int(cr.rsplit("/", 1)[1])
                while not estado["parar"]:
                    d = r.read(65536)
                    if not d:
                        break
                    escreve(fd, d)
                    pos += len(d)
                    agora = time.time()
                    if agora - t_log >= 30:
                        t_log = agora
                        log("download", nome, pos, "de", total)
                r.close()
            except HTTPError as e:
                if e.status == 416:          # pediu depois do fim: acabou
                    break
                quedas += 1
                log("download", nome, "HTTP", e.status, "em", pos)
                if quedas > TENTATIVAS or e.status in (403, 404, 410):
                    status("erro", "download %s: HTTP %s" % (nome, e.status))
                    return
                time.sleep(1)
                continue
            except (RequestError, OSError) as e:
                if isinstance(e, BrokenPipeError):
                    return                   # o ffmpeg saiu
                quedas += 1
                log("download", nome, "queda", repr(e), "em", pos)
                if quedas > TENTATIVAS:
                    status("erro", "download %s: %s" % (nome, e))
                    return
                time.sleep(1)
                continue
            if pos > antes:
                quedas = 0
            if not total and pos - antes < fim - antes + 1:
                break                        # sem tamanho: o pedaco veio curto, era o fim
            if pos == antes:
                quedas += 1
                if quedas > TENTATIVAS:
                    status("erro", "download %s: o servidor nao manda mais nada" % nome)
                    return
        rc = 0
        log("download", nome, "fim", pos, "de", total)
    except BrokenPipeError:
        pass
    finally:
        estado["rc_" + nome] = rc
        try:
            os.close(fd)
        except OSError:
            pass


class Mux:
    """Pacotes H/V/A no stdout (fd 1), um escritor por vez."""

    def __init__(self):
        self.trava = threading.Lock()
        self.quebrou = False

    def poe(self, tipo, dados):
        n = len(dados)
        with self.trava:
            if self.quebrou:
                return False
            try:
                escreve(1, tipo + bytes((n & 255, n >> 8 & 255, n >> 16 & 255)) + dados)
            except OSError:
                self.quebrou = True      # o PLAY fechou a caixa
                return False
        return True


def repassa(mux, tipo, fd):
    while True:
        try:
            d = os.read(fd, 65536)
        except OSError:
            break
        if not d or not mux.poe(tipo, d):
            break
    os.close(fd)


def main():
    if len(sys.argv) < 2:
        status("erro", "uso: YTPLAY.PY LINK [LARGURA] [QPS] [QUALIDADE] [HZ] [CANAIS]")
        return 2
    url = sys.argv[1]
    largura = numero(2, 480, 160, 800) // 2 * 2
    qps = numero(3, 15, 1, 30)
    qualidade = numero(4, 7, 2, 31)
    hz = numero(5, 0, 0, 48000)
    canais = numero(6, 2, 1, 2)
    if hz and hz < 8000:
        hz = 0

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

    log("inicio", url, largura, qps, qualidade, hz, canais)
    opts = {
        "format": FORMATO_SOM if hz else FORMATO,
        "quiet": True,
        "no_warnings": True,
        "noprogress": True,
        "noplaylist": True,
        "cachedir": False,
        "logger": Log(),
    }
    estado = {"parar": False}
    ff = None
    leitores = []
    rc = 1
    try:
        with yt_dlp.YoutubeDL(opts) as ydl:
            info = ydl.extract_info(url, download=False)
            if not info:
                return 1
            fmts = info.get("requested_formats") or [info]
            fv = fmts[0]
            fa = fmts[1] if len(fmts) > 1 else None
            status("titulo", info.get("title") or "?")
            status("duracao", int(info.get("duration") or 0))
            status("formato", fv.get("format_id") or "?", fv.get("vcodec") or "?",
                   "%sx%s" % (fv.get("width") or "?", fv.get("height") or "?"))
            som_junto = fa is None and (fv.get("acodec") or "none") != "none"
            tem_som = hz and (fa is not None or som_junto)
            if hz:
                if fa is not None:
                    status("som", fa.get("format_id") or "?", fa.get("acodec") or "?")
                elif som_junto:
                    status("som", fv.get("format_id") or "?", fv.get("acodec") or "?")
                else:
                    status("som", "nenhum")
            log("formatos", fv.get("format_id"), fa.get("format_id") if fa else "-",
                "som" if tem_som else "sem som")

            vin_r, vin_w = os.pipe()
            cmd = [FFBIN, "-hide_banner", "-loglevel", "error", "-i", "pipe:0"]
            passa = []
            ain_r = ain_w = -1
            if tem_som and fa is not None:
                ain_r, ain_w = os.pipe()
                cmd += ["-i", "pipe:%d" % ain_r]
                passa.append(ain_r)
            saida_v = ["-map", "0:v:0", "-vf", "fps=%d,scale=%d:-2" % (qps, largura),
                       "-q:v", str(qualidade), "-f", "mjpeg"]
            mux = None
            if hz:
                mux = Mux()
                mux.poe(b"H", ("SOM %d %d" % (hz if tem_som else 0, canais)).encode())
                vout_r, vout_w = os.pipe()
                cmd += saida_v + ["pipe:1"]
                aout_r = aout_w = -1
                if tem_som:
                    aout_r, aout_w = os.pipe()
                    cmd += ["-map", "%d:a:0" % (1 if fa is not None else 0),
                            "-ac", str(canais), "-ar", str(hz), "-c:a", "pcm_s16le",
                            "-f", "s16le", "pipe:%d" % aout_w]
                    passa.append(aout_w)
                ff = subprocess.Popen(cmd, stdin=vin_r, stdout=vout_w, stderr=subprocess.PIPE,
                                      pass_fds=passa)
                os.close(vout_w)
                leitores = [threading.Thread(target=repassa, args=(mux, b"V", vout_r),
                                             daemon=True)]
                if tem_som:
                    os.close(aout_w)
                    leitores.append(threading.Thread(target=repassa, args=(mux, b"A", aout_r),
                                                     daemon=True))
            else:
                ff = subprocess.Popen(cmd + ["-an"] + saida_v + ["pipe:1"], stdin=vin_r,
                                      stdout=1, stderr=subprocess.PIPE)
                leitores = []
            os.close(vin_r)
            if ain_r >= 0:
                os.close(ain_r)
            threading.Thread(target=repassa_erros, args=(ff.stderr,), daemon=True).start()
            for t in leitores:
                t.start()
            baixadores = [threading.Thread(target=baixa, args=(ydl, fv, vin_w, "imagem", estado),
                                           daemon=True)]
            if ain_w >= 0:
                baixadores.append(threading.Thread(target=baixa,
                                                   args=(ydl, fa, ain_w, "som", estado),
                                                   daemon=True))
            for t in baixadores:
                t.start()
            # join com tempo: o SIGINT (KeyboardInterrupt) so chega na thread principal
            for t in baixadores:
                while t.is_alive():
                    t.join(0.5)
            rc = max(estado.get("rc_imagem", 1), estado.get("rc_som", 0))
    except KeyboardInterrupt:
        log("SIGINT (o PLAY pediu para parar)")
        estado["parar"] = True
        rc = 130
    except Exception as e:  # noqa: BLE001 - mostra o erro no PLAY
        log("excecao", repr(e))
        if not str(e).startswith("ERROR"):
            status("erro", e)
        rc = 1
    ffrc = -1
    if ff is not None:
        try:
            if rc == 130:
                ff.kill()
            ffrc = ff.wait(timeout=60)
            for t in leitores:
                t.join(10)
        except (KeyboardInterrupt, subprocess.TimeoutExpired):
            ff.kill()
            ffrc = ff.wait()
    log("fim download", rc, "ffmpeg", ffrc)
    status("fim", rc, ffrc)
    return 0 if rc == 0 and ffrc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
