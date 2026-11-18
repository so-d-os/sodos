# 02 — O instalador do Claude Desktop no SODOS

Registro do que o instalador (`Claude Setup.exe`, o `win32-bootstrapper` em
Go 1.25) pede ao sistema e de como cada pedido foi atendido. Testado no QEMU
em 2026-11-18 com o arquivo `Claude Setup(2).exe` de 2026-10-01.

## O que o instalador faz

1. Sobe o runtime do Go: reserva gigabytes de endereço (`VirtualAlloc`), cria
   threads, timers e uma porta de conclusão; lê o relógio direto de
   `KUSER_SHARED_DATA` (0x7FFE0008) e o TLS direto de `gs:[0x1480+n]`.
2. Detecta o sistema: versão/edição (registro), arquitetura, elevação
   (token), S Mode, sideloading, serviços em conflito.
3. Abre a janela de progresso: splash 400×300 animado (GDI, DIB de 32 bits).
4. Baixa o pacote: HTTPS (TLS 1.3 do Go sobre Winsock sobreposto) de
   `api.anthropic.com/.../msix/latest/redirect` para `%TEMP%` (~300 MB).
5. Relança a si mesmo "elevado" (`ShellExecuteEx runas ... --elevated
   --msix-path ...`) e espera o filho.
6. O filho confere as assinaturas (`WinVerifyTrust`, `CryptQueryObject` no
   próprio EXE e no `.msix`) e exige que o **certificado do pacote seja o
   mesmo do instalador**.
7. Instala: `PackageManager.FindPackages`, `Uri.CreateUri(file:///...)`,
   `PackageManager.AddPackageAsync`.

## Como o SODOS atende

| Pedido | Solução |
|---|---|
| GS = TEB | a base do GS de uma thread do Windows é o TEB; `TEB+0`/`TEB+8` guardam CPU e tarefa para o kernel (`kthread.c`) |
| `KUSER_SHARED_DATA` | página alocada em 0x7FFE0000 só com EXE do Windows rodando; o IRQ do timer atualiza |
| reserva de memória | bloco de RAM de verdade, commit zera sob demanda (`vm.c`) |
| elevação | o token diz "elevado": no SODOS todo programa é administrador |
| relançar a si mesmo | processo filho aninhado (`k_exec`) com handle e código de saída (`proc.c`) |
| arquivo de 300 MB em `%TEMP%` | fica em RAM, visível ao processo filho (`file.c`) |
| certificado TLS / assinaturas | sem autoridades raiz nem Authenticode: falha por padrão; `SET WINE_NOVERIFY=1` aceita sem conferir e avisa. O assinante é lido de verdade (PKCS#7 do PE e do `AppxSignature.p7x`) |
| `AddPackageAsync` | extrai o `.msix` (ZIP) para `D:\WINE\APPS\<nome completo>` (`pkg.c`, `zip.c`) |

## Resultado do teste

- Captura do splash no desktop do SODOS: [img/claude-setup-sodos.png](img/claude-setup-sodos.png).
- Detecção, janela, download (299 MB em ~2,5 min pela rede do SODOS), filho
  "elevado" e leitura das assinaturas funcionam.
- **Este instalador (1º/out/2026) recusa o pacote atual**: ele foi assinado
  com o certificado da Anthropic válido de 2025-10-14 a 2026-10-20 (série
  ...12d31a50); o MSIX "latest" vem assinado com o certificado renovado,
  2026-10-08 a 2027-10-28 (série ...2ec875b8), mesma identidade
  ("Anthropic, PBC", subject e emissor idênticos), outra chave. A mensagem é
  a do próprio instalador: "MSIX signer does not match bootstrapper". É do
  arquivo, não do SODOS; precisa de um `Claude Setup.exe` atual.
- Com o diagnóstico `WINE_SIGTEST=1` (mesma assinatura para os dois) o fluxo
  segue até o fim: o pacote `Claude_5.11492.5.0_x64__pzs8sxrjxfjjc` (174 arquivos,
  733 MB, `claude.exe` de 250 MB) é extraído sem falhas e o log do instalador
  termina com "Claude Setup completed successfully", código de saída 0. No
  QEMU a extração levou 47 min por causa do disco ATA em PIO (0,26 MB/s).
- O aplicativo instalado é Electron (Chromium): **não roda** no `WINE.DLL`
  atual. Rodá-lo é outro projeto (GPU, sandbox, COM, dezenas de DLLs).

## Como repetir

```
cp "/mnt/c/Users/<u>/Downloads/Claude Setup.exe" wine/build/tests/CLAUDE.EXE
NOVERIFY=1 sh wine/scripts/win-test.sh start      # desktop + instalador
sh wine/scripts/win-test.sh shot                  # captura, serial, ClaudeSetup.log
sh wine/scripts/win-test.sh stop
```
