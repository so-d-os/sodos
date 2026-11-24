# install — o que vai para onde no D: do SODOS

Tudo o que o `D:` recebe está aqui, numa lista por componente, em `componentes/<nome>.lst`. O `install.sh` lê as listas e copia para o disco. É o mesmo caminho para todos os destinos:

| Destino | Como chega aqui |
|---|---|
| `disk.img` do QEMU | `make disk-files` (dentro de `src13/`) |
| SSD desta máquina (SODOSHD) | `make send` |
| PC com Windows (WSL) | `make sendpc` |
| Só as listas, sem compilar o c_compiler | `make instalar` ou `sh install/install.sh <disco>` |

O Makefile só compila: o `c_compiler` (`.EXE`, `.DLL`, `.OBJ`) e os arquivos gerados (`make install-gerados`: as miniaturas `.THM` do desktop e os ícones `.ICN` dos temas, em `src13/c_compiler/build/install`). Para mudar o que vai para o `D:`, mude a lista, não o Makefile.

## Rodar

```sh
cd src13
make instalar                       # todos os componentes, no disk.img
make instalar C="claude wsl"        # só esses
make instalar DISK=/mnt/sodoshd     # numa FAT32 já montada
make instalar N=1                   # só mostra o que faria
```

Ou direto, de qualquer pasta: `sh install/install.sh [-n] <disco> [componente...]`. O `<disco>` é uma imagem FAT (gravada com o mtools) ou a pasta onde a FAT32 está montada (gravada pelo `src13/tools/dirfs`, que só copia o que mudou). Com o QEMU aberto no `disk.img`, o script recusa: o SO regravaria a FAT por cima. Os componentes rodam em ordem alfabética, e nada é apagado do disco.

## Componentes

| Lista | O que instala |
|---|---|
| `aparencia.lst` | Temas em `D:\SODOS\ICONS` (PNG + ICN), papéis de parede em `D:\SODOS\WALLS`, fontes TTF em `D:\SODOS\FONTS` |
| `claude.lst` | `CLAUDE.md` do Claude Code no WINE (`D:\WINE\USERS\SODOS\.claude`, fonte `src13/docs/CLAUDE-SODOS.md`) e o `BASH.EXE` |
| `config.lst` | `D:\AGENT.CFG` (se existir; tem a chave da API) e `D:\NET\WIFI.SIM` |
| `desktop.lst` | `D:\USER\DEFAULT\DESKTOP`: o que está em `src13/content` e as miniaturas `.THM` |
| `drivers.lst` | DLLs de `D:\SODOS\DRV` e firmware em `D:\SODOS\FW` |
| `firefox.lst` | `D:\FIREFOX` (pelo script do port) e o atalho `FIREFOX.EXE` no desktop |
| `lib.lst` | Bibliotecas `.so` em `D:\LIB` |
| `programas.lst` | Raiz do `D:`: `.EXE`, `.DLL`, compilador, libc, headers (`D:\SYS`), exemplos |
| `qemu.lst` | `D:\UTILS\QEMU` (pelo script do port) |
| `sodos.lst` | Programas do sistema em `D:\SODOS`, `LINKEXT.CFG` (sem sobrescrever) e os READMEs |
| `src.lst` | Fontes para recompilar no SODOS: `D:\SRC\{MC,TOP,GIT,WIFI,CMD\XCOPY}` e `D:\DEVELOP\MAKE` |
| `wine.lst` | `GIT.EXE` em `D:\WINE\BIN` |
| `wsl.lst` | `D:\WSL.EXE`, o script `D:\WSL\SODOS` e um aviso se falta a imagem `D:\WSL\DEBIAN.IMG` |

## Formato da lista

Uma ação por linha; `#` no começo é comentário. As origens são relativas à raiz do repositório; os destinos são caminhos do SODOS, sempre no `D:`. Um `|` separa a mensagem do fim da linha.

```
copia    <origem>  <destino>  [opções]  [| mensagem]
pasta    <destino>
confere  <destino>  | <mensagem>
executa  <script>  [argumentos]
```

**copia** copia e cria as pastas que faltam. Sobrescreve sempre, a não ser com `preserva`.

- Destino terminado em `\` é uma pasta. A origem pode ter `*` e `?`, e cada arquivo entra com o próprio nome **em maiúsculas** (8.3, como o resto do `D:`). Sem o `\` no fim, o destino é o nome do arquivo, escrito como deve ficar.
- `arvore`: a origem é uma pasta, e tudo o que tem dentro (com subpastas) vai para a pasta do destino.
- `nome-original`: não passa para maiúsculas (`libc.so.6`).
- `exceto=<nome>`: pula os arquivos com esse nome (aceita `*`). Pode repetir.
- `opcional`: se a origem não existe (não foi compilada), só avisa, com a mensagem do `|` quando tem uma. Sem ela, a falta é erro e o `make` para.
- `preserva`: só copia se o destino ainda não existe. Serve para configuração que o usuário edita no SODOS.

**pasta** cria a pasta vazia, se faltar.

**confere** não copia nada: mostra a mensagem se o arquivo (aceita `*`) não está no disco. Serve para o que vai à mão, como a imagem de 2 GB do WSL.

**executa** roda `sh <script> <disco> [argumentos]` a partir da raiz do repositório. Serve para o que tem regra própria e não cabe numa lista, como o Firefox e o QEMU. O script recebe a imagem ou a pasta e grava com `mcopy`/`mmd`/`mdir -i "<disco>@@512"`, como os `copy-*-disk.sh`. Se falhar, conta como erro.

Exemplo:

```
copia    src13/docs/CLAUDE-SODOS.md      D:\WINE\USERS\SODOS\.claude\CLAUDE.md
copia    src13/c_compiler/out/*.EXE      D:\   exceto=TEST.EXE
copia    src13/content/icons             D:\SODOS\ICONS\   arvore
copia    src13/content/LINKEXT.CFG       D:\SODOS\LINKEXT.CFG   preserva
copia    src13/wsl/build/WSL.EXE         D:\WINE\BIN\BASH.EXE   opcional
confere  D:\WSL\DEBIAN.IMG  | copie src13/wsl/build/image/* para D:\WSL a mão
executa  src13/qemu/scripts/copy-qemu-disk.sh
```

Um componente novo é só um arquivo `.lst` novo em `componentes/`.
