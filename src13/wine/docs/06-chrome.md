# 06 — Google Chrome no SODOS

O `INSTCHRM.EXE` baixa o Google Chrome (x64, canal estável) do Google e o
instala em `D:\APPS\CHROME\<versão>`. Ele grava o `D:\RODACHROME.BAT` e o
atalho **CHROME** no desktop, que abrem o Chrome pelo WINE.DLL.

## Uso

```
INSTCHRM            pergunta a versão ao Google, baixa, confere e instala
RODACHROME          abre o Chrome (SET WINE=1 e as opções do Chromium)
```

| Opção | O que faz |
|---|---|
| `INSTCHRM -n` | só mostra a versão que o Google oferece (e o endereço) |
| `INSTCHRM -r` | instala de novo mesmo se a versão já está lá |
| `INSTCHRM -f ARQ` | instala de um instalador já baixado, sem rede (o `NNN_chrome_installer_uncompressed.exe`) |
| `INSTCHRM -b` | só grava o `D:\RODACHROME.BAT` e o atalho, para a versão já instalada |
| `SET INSTDBG=1` | mostra o cabeçalho do 7z e o LZMA (2 = cada símbolo do LZMA) |

Atualizar é rodar o `INSTCHRM` de novo (com o Chrome fechado): se o Google
oferece outra versão, ela é instalada numa pasta nova ao lado, o
`RODACHROME.BAT` passa a abri-la e as pastas das versões antigas são
apagadas. Se a pasta da versão some, o `RODACHROME.BAT` chama o `INSTCHRM`
sozinho e depois abre a versão que ele instalou.

Precisa de rede, de ~520 MB livres no `D:` e de ~260 MB de memória livre (o
download vai para a memória antes de ir para o disco). O pacote não fica no
disco. Na VM de teste (QEMU, disco IDE) foram ~2 min de download e ~35 min
gravando; o disco é o gargalo.

## De onde vem e o que ele faz

1. **Versão e endereço**: `POST https://tools.google.com/service/update2`, o
   protocolo do Google Update (Omaha 3.0), com o appid do Chrome
   `{8A69D345-D564-463C-AFF1-A69D9E530F96}` e `ap="x64-stable-multi-chrome"`.
   A resposta traz a versão, o endereço em `https://dl.google.com/release2/chrome/...`,
   o tamanho e o SHA-256 do instalador. Hoje é o
   `158.0.8127.89_chrome_installer_uncompressed.exe` (536 MB).
2. **Índice**: dois pedidos com `Range`: os primeiros 128 KB (cabeçalho PE e
   diretório de recursos, onde está o recurso `BN/CHROME.7Z`) e os últimos
   256 KB do 7z (o cabeçalho, comprimido com LZMA, com a lista de arquivos).
3. **Download para a memória**: `GET` com `Accept-Encoding: gzip` (o
   `dl.google.com` manda 252 MB em vez de 536), guardado em blocos de 4 MB.
   Baixar e gravar ao mesmo tempo não funciona num disco lento: a rede do
   SODOS só lê a placa dentro do `recv` (`net/net.c`, `api_recv`), os
   pacotes que chegam enquanto o programa grava se perdem, o servidor
   retransmite cada vez mais devagar e o `recv` desiste com 8 s sem dados.
   Na VM a ne2000 ainda parava de receber de vez (nem o DNS respondia). Sem
   memória para o pacote, o INSTCHRM volta a gravar enquanto baixa.
4. **Extração**: inflate em fluxo da memória, e cada arquivo do `Chrome-bin`
   é gravado enquanto os bytes dele passam: no 7z do instalador
   "uncompressed" os arquivos estão sem compressão (método Copy). Só os dois
   `.sig` (BCJ2 + LZMA) são decodificados, na memória.
5. **Conferência**: CRC32 de cada arquivo (do 7z) e SHA-256 do pacote inteiro
   (do Google). Só depois disso o `RODACHROME.BAT` é regravado para a versão
   nova. Se algo falha, a pasta da versão nova é apagada e a anterior
   continua como estava. Conexão que cai no download: tenta de novo, até 3
   vezes.

O `setup.exe` do Google não roda (serviço do Google Update, registro,
tarefas agendadas, atalhos do shell) e não é usado.

Resultado:

```
D:\APPS\CHROME\158.0.8127.89\chrome.exe, chrome_proxy.exe
D:\APPS\CHROME\158.0.8127.89\chrome.dll, chrome_elf.dll, resources.pak, Locales\..., ...
```

O `chrome.exe` fica na pasta da versão, e não um nível acima como no
`Chrome-bin`: ele importa o `chrome_elf.dll`, que no Windows é achado pelo
manifesto da pasta da versão (assembly privado `158.0.8127.89.manifest`), e
o WINE.DLL só procura DLLs na pasta do EXE e no `SYSTEM32` (`dll/pe.c`,
`find_file`). Com o `chrome.exe` na raiz o Chrome saía logo com 0xC0000139
("DLL ausente: chrome_elf.dll"). Ao lado das DLLs ele roda como num build de
desenvolvimento: sem `<exe>\<versão>\chrome.dll`, carrega o `chrome.dll` da
própria pasta.

## RODACHROME.BAT

Gravado pelo `INSTCHRM` com a versão instalada (instalar de novo regrava;
edições se perdem):

```bat
IF NOT EXIST D:\APPS\CHROME\158.0.8127.89\chrome.exe GOTO INSTALA
SET WINE=1
SET HIMEM=80
SET WINEUDSAVE=D:\CHROME.SAV
SET WINE_ARGS_EXE=chrome.exe
SET WINE_ARGS=--no-sandbox --user-data-dir=Z:\CHROMEUD --no-first-run --no-default-browser-check --disable-gpu ...
SET WINE_ARGS2=--disable-3d-apis --disable-webgl --disable-webgpu ... --ipc-connection-timeout=300
WIN D:\APPS\CHROME\158.0.8127.89\chrome.exe
GOTO FIM
:INSTALA
D:\INSTCHRM.EXE
IF ERRORLEVEL 1 GOTO FIM
CALL D:\RODACHROME.BAT
:FIM
```

- `WINE_ARGS_EXE=chrome.exe`: as opções só entram no `chrome.exe` de cima
  (`main.c`); os processos filhos recebem do próprio Chrome o que precisam.
- `--user-data-dir=Z:\CHROMEUD`: perfil no ramdisk (rápido, sem gravar no
  `D:`). Com `SET WINEUDSAVE=D:\CHROME.SAV` o processo de cima restaura esse
  perfil na partida e o salva a cada minuto e na saída (`dll/profsave.c`):
  o login do Google volta no próximo boot.
- As opções de GPU são as do Claude Desktop (`RODACLAUDE.BAT`): sem
  aceleração 3D, o Chromium desenha por software.
- `HIMEM=80`: até 80% da RAM acima de 4 GB para programas de 64 bits.
- Atalho: `D:\USER\DEFAULT\DESKTOP\CHROME.LNK` (primeira linha =
  `D:\RODACHROME.BAT`), criado só se não existe.

Cada processo do Chromium abre uma janela de console do WINE (como no
Claude Desktop); a do navegador é a "Google Chrome".

## Limites

- Sem atualização automática (não há Google Update): rode o `INSTCHRM`.
- Se o Google voltar a mandar o instalador comprimido (7z com LZMA2, ou
  BCJ2 em arquivos grandes), o INSTCHRM para com "método de compressão não
  suportado" ou "pasta comprimida grande demais": LZMA e BCJ2 só na memória
  e até 64 MB por pasta.
- O certificado de `tools.google.com` e `dl.google.com` tem que conferir no
  `TLS.DLL` (`tls_cert_ok`); se não confere, o INSTCHRM não baixa.

## Código

- `src13/utils/instchrm.c` (regra `INSTCHRM.EXE` no `c_compiler/Makefile`;
  vai para a raiz do `D:` pela lista `install/componentes/programas.lst`).
- O `cc` do SODOS avalia os operandos de `+` em outra ordem que o gcc. Duas
  chamadas que leem o mesmo fluxo (range coder, bits do inflate) numa
  expressão só dão resultado diferente; o LZMA quebrava assim
  (`rep0 += (direct() << 4) + rtree()`). Uma leitura por instrução.
