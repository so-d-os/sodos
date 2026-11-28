# SETUP — instalador gráfico do SODOS

`SETUP.EXE` abre uma janela no WIN com um botão: **Instalar**. Ele copia cada arquivo do
pacote para a pasta certa e mostra uma barra de progresso (por bytes). No fim, põe
`D:\SODOS\BIN` no PATH do `AUTOEXEC.BAT` e do sistema.

| Arquivo | O que é |
|---|---|
| `install.c` | a lista `ITENS` ({origem, destino, opções}) e a função `install()` |
| `install.h` | a interface do `install()` e do progresso |
| `setup.c` | a janela (GUI.DLL + widgets), o botão e a barra |
| `AUTOEXEC.BAT` | modelo, gravado só se o disco ainda não tem um |
| `empacota.sh` | monta o pacote: `SETUP.EXE` + `ARQUIVOS\` |
| `BUILD.BAT` | compila no SODOS |

## Pastas

- `D:\SODOS\BIN`: comandos (git, make, top, wifi, cc, link...)
- `D:\SODOS`: programas do sistema (BAT, PENDRIVE, WSL e os painéis do WIN)
- `D:\SODOS\DEV\SRC\{LIBC,CC,LINK,TLS,GUI,IMG,BROWSER}` e `D:\SODOS\DEV\EXEMPLOS`
- `D:\`: o que o código ainda procura pelo caminho fixo (EXPLORER, BROWSER, VIDPLAY,
  IMAGEVW, WEBCAMV) e o kit do cc (CRT0.OBJ, KERNEL.IMP, as DLLs, os `.H` e `SYS\`).
  Quando o código mudar, mude só o destino no `install.c`.

## Uso

```sh
cd /d/SOURCE/SODOS/setup
sh /d/WSL/SODOS cc -I /d/ -L /d/ -o /d/SOURCE/SODOS/setup/SETUP.EXE /d/SOURCE/SODOS/setup/setup.c /d/SOURCE/SODOS/setup/install.c /d/GUI.DLL
sh empacota.sh /d /d/SETUP        # pacote em D:\SETUP, com os arquivos de D:\ de hoje
```

No WIN: `D:\SETUP\SETUP.EXE` instala de verdade. `D:\SETUP\SETUP.EXE D:\TEMP\TESTE` grava
tudo debaixo de `D:\TEMP\TESTE` (teste) e não mexe no PATH do sistema.

Uma linha nova na lista (`{"NOME.EXE", BIN, 0},`) já entra no `empacota.sh`: ele lê as
mesmas linhas do `install.c`.
