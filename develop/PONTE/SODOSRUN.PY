#!/usr/bin/python3
"""SODOSRUN.PY - roda uma linha do prompt do SODOS a partir da VM Linux do WSL,
pela ponte de arquivos do PONTE.EXE (que precisa estar aberto numa tela do SODOS).

Uso:
  python3 /d/DEVELOP/PONTE/SODOSRUN.PY [-C PASTA] [-t SEGUNDOS] COMANDO...

  -C PASTA     pasta no SODOS onde o comando roda (D:\\X\\Y ou /d/x/y).
               Padrao: a pasta atual, se estiver numa unidade do SODOS
               (/d/develop/agent2 -> D:\\develop\\agent2).
  -t SEGUNDOS  espera maxima pela resposta (padrao 600).

Caminhos no comando: /d/x/y vira D:\\x\\y (o BASH.EXE do WSL troca os
caminhos do Windows por /d/... antes de chegar aqui; assim eles voltam).
Nao troca com -r nem em "WSL ..." (o WSL exec quer caminhos do Linux).
| > < no comando sao do prompt do SODOS: para pipe no Linux, use um script.

Sai com o codigo do comando no SODOS (0..255; outro valor vira 1),
124 se o tempo acabou e 125 se o PONTE.EXE nao pegou o pedido.
A fila e /d/DEVELOP/PONTE/FILA; SODOS_FILA=/x/y troca.

Protocolo (veja PONTE.C):
  PED.TXT    PONTE 1 <id>\\n<pasta>\\n<comando>\\n             (CP850)
  RESP.TXT   PONTE 1 <id> <codigo> <bytes> <cortou>\\n<saida>
"""
import argparse
import os
import random
import sys
import time

FILA = os.environ.get("SODOS_FILA", "/d/DEVELOP/PONTE/FILA")
PEGA_S = 5.0      # tempo para o PONTE.EXE pegar o pedido
TIQUE_S = 0.1


def para_sodos(caminho):
    """/d/x/y, /mnt/d/x/y ou /mnt/sodos/d/x/y -> D:\\x\\y ("" fora do SODOS)."""
    partes = [p for p in os.path.abspath(caminho).split("/") if p]
    if partes[:2] == ["mnt", "sodos"]:
        partes = partes[2:]
    elif partes[:1] == ["mnt"]:
        partes = partes[1:]
    if not partes or len(partes[0]) != 1 or not partes[0].isalpha():
        return ""
    return partes[0].upper() + ":\\" + "\\".join(partes[1:])


def palavras_sodos(cmd):
    """O BASH.EXE do WSL troca D:\\X por /d/X em tudo que o Claude Code manda.
    Aqui volta: cada palavra /d/..., /mnt/d/... vira D:\\... (opcoes como
    /w ficam como estao, porque nao tem a barra depois da letra)."""
    saida = []
    for p in cmd.split(" "):
        if (len(p) >= 3 and p[0] == "/" and p[1].isalpha() and p[2] == "/") or \
           p.startswith("/mnt/"):
            q = para_sodos(p)
            if q:
                p = q
        saida.append(p)
    return " ".join(saida)


def le_resposta(caminho):
    """(id, codigo, cortou, dados) se RESP.TXT esta inteiro; senao None."""
    try:
        with open(caminho, "rb") as f:
            bruto = f.read()
    except OSError:
        return None
    fim = bruto.find(b"\n")
    if fim < 0:
        return None
    cab = bruto[:fim].decode("latin-1").split()
    if len(cab) != 6 or cab[0] != "PONTE" or cab[1] != "1":
        return None
    try:
        codigo, n, cortou = int(cab[3]), int(cab[4]), int(cab[5])
    except ValueError:
        return None
    dados = bruto[fim + 1:]
    if len(dados) < n:
        return None
    return cab[2], codigo, cortou, dados[:n]


def erro(msg, codigo):
    print("sodos-run: " + msg, file=sys.stderr)
    return codigo


def main():
    ap = argparse.ArgumentParser(
        prog="sodos-run",
        description="Roda uma linha do prompt do SODOS pela ponte do PONTE.EXE.")
    ap.add_argument("-C", dest="pasta", help="pasta no SODOS (D:\\X ou /d/x)")
    ap.add_argument("-t", dest="tempo", type=float, default=600.0,
                    help="espera maxima em segundos (padrao 600)")
    ap.add_argument("-r", dest="cru", action="store_true",
                    help="nao troca /d/x por D:\\x no comando")
    ap.add_argument("comando", nargs=argparse.REMAINDER)
    a = ap.parse_args()

    cmd = " ".join(a.comando).strip()
    # WSL exec manda o comando ao Linux: la os caminhos sao /d/...
    if not a.cru and not cmd.lower().startswith("wsl "):
        cmd = palavras_sodos(cmd)
    if not cmd:
        ap.error("falta o comando")
    pasta = a.pasta if a.pasta is not None else os.getcwd()
    if pasta.startswith("/"):
        pasta = para_sodos(pasta)
    if "\n" in cmd or "\r" in cmd or "\n" in pasta:
        return erro("o comando e a pasta tem de ser uma linha so", 2)
    try:
        cmd_b = cmd.encode("cp850")
        pasta_b = pasta.encode("cp850")
    except UnicodeEncodeError:
        return erro("caractere que nao existe no CP850 do SODOS", 2)
    if len(cmd_b) > 250:
        return erro("comando longo demais (o prompt do SODOS aceita 255)", 2)

    os.makedirs(FILA, exist_ok=True)
    ped = os.path.join(FILA, "PED.TXT")
    tmp = os.path.join(FILA, "PED.TMP")
    resp = os.path.join(FILA, "RESP.TXT")

    # um pedido por vez: espera o anterior sair da fila
    t0 = time.time()
    while os.path.exists(ped):
        if time.time() - t0 > PEGA_S:
            return erro("ha um pedido parado em %s: o PONTE.EXE esta aberto "
                        "numa tela do SODOS?" % ped, 125)
        time.sleep(TIQUE_S)

    rid = "%x%04x" % (int(time.time() * 1000) & 0xFFFFFFFF, random.getrandbits(16))
    with open(tmp, "wb") as f:
        f.write(b"PONTE 1 " + rid.encode() + b"\n" + pasta_b + b"\n" + cmd_b + b"\n")
    os.replace(tmp, ped)

    # o PONTE.EXE apaga o PED.TXT quando pega o pedido
    t0 = time.time()
    while os.path.exists(ped):
        if time.time() - t0 > PEGA_S:
            try:
                os.remove(ped)
            except OSError:
                pass
            return erro("o PONTE.EXE nao pegou o pedido em %d s; abra uma tela "
                        "livre no SODOS e rode:  ponte" % PEGA_S, 125)
        time.sleep(TIQUE_S)

    t0 = time.time()
    while True:
        r = le_resposta(resp)
        if r and r[0] == rid:
            break
        if time.time() - t0 > a.tempo:
            return erro("sem resposta em %d s (o comando ainda pode estar "
                        "rodando no SODOS)" % a.tempo, 124)
        time.sleep(TIQUE_S)
    try:
        os.remove(resp)
    except OSError:
        pass

    _, codigo, cortou, dados = r
    texto = dados.decode("cp850").replace("\r\n", "\n")
    sys.stdout.write(texto)
    if texto and not texto.endswith("\n"):
        sys.stdout.write("\n")
    sys.stdout.flush()
    if cortou:
        print("sodos-run: saida cortada em %d bytes" % len(dados), file=sys.stderr)
    return codigo if 0 <= codigo <= 255 else 1


if __name__ == "__main__":
    sys.exit(main())
