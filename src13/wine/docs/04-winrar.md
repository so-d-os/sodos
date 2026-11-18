# WinRAR 7.23 no SODOS

Teste de 2026-11-18 no QEMU (`wine/scripts/vm.sh`, porta 8803) com `SET WINE=1`.

## O que funciona

1. **Instalador gráfico** (`winrar-x64-723.exe`, sem `/S`): o diálogo de
   instalação aparece no WIN; **Install** extrai tudo para
   `D:\WINE\PROGRAM FILES\WinRAR` (30 arquivos, 9,9 MB). Em seguida o filho
   `uninstall.exe /setup` mostra o diálogo de opções (associações, atalhos);
   **OK** leva à tela final "WinRAR has been successfully installed"; **Done**
   encerra os dois processos com código 0.
2. **`Rar.exe`** (console) instalado: `a`, `t` e `l` conferidos
   (`Rar.exe a D:\WINE\T.RAR D:\HELLO.EXE D:\HELLOW.EXE`, depois `t` = "All OK").
3. **`WinRAR.exe`** (interface): a janela principal "C:\ (evaluation copy)"
   abre com a lista de arquivos (ListView owner-data: nome, tamanho, data),
   seleção e a barra de status ("Selected 405 bytes in 1 file / Total ...").

## O que foi preciso

- `dll/dlg.c`, `dll/ctl.c`, `dll/user.c`, `dll/gdi.c`, `dll/text.c`:
  diálogos de recurso (DLGTEMPLATE/EX), controles padrão, várias janelas de
  topo no mesmo canvas do WIN, texto TrueType (Inter).
- `dll/comctl.c`: `SysListView32` (modo relatório, `LVS_OWNERDATA` com
  `LVN_GETDISPINFOW`, seleção, teclado, duplo clique), `ToolbarWindow32`,
  `msctls_statusbar32`, `ReBarWindow32`, mensagens `CBEM_*` do ComboBoxEx32.
- `dll/uimisc.c`: mensagens registradas, propriedades de janela, subclasses do
  comctl32 (`SetWindowSubclass`/`DefSubclassProc`), menus de recurso
  (guardados, ainda sem barra desenhada), aceleradores, área de transferência
  dentro do processo, barras de rolagem guardadas, ImageList (só contagem),
  uxtheme sem tema, gdiplus que falha limpo, e os ordinais do comctl32
  (380/381/410-413), shell32 (100) e shlwapi (219).
- Registro: `ProgramFilesDir` = `D:\WINE\PROGRAM FILES` (o destino padrão do
  instalador era relativo).
- `user.c`: duplo clique (`WM_LBUTTONDBLCLK`) para os controles embutidos.

## Ainda falta

- Barra de menus e menus popup desenhados (o menu existe, mas não aparece).
- Ícones (ImageList/`DrawIconEx`): toolbar e lista mostram só texto.
- A toolbar e a barra de endereço do WinRAR ainda não aparecem acima da lista.
- Diálogos comuns (`COMDLG32`: abrir/salvar arquivo) e `PropertySheet`
  (Opções do WinRAR).

## Reproduzir

```sh
KEEP=1 sh wine/scripts/vm.sh start win        # reaproveita o disco com o WinRAR
sh wine/scripts/vm.sh cmd "SET WINE=1"
sh wine/scripts/vm.sh launch "D:\WINE\PROGRAM FILES\WinRAR\WinRAR.exe"
sh wine/scripts/vm.sh shot winrar
```

`GUI=1` no `start` abre a janela do QEMU. Para mapear um crash dentro do
`WINE.DLL`: `LINK64_MAP=1 make build/WINE.DLL 2> build/wine.map` e somar o
"wine_driver + X" do log ao endereço de `wine_driver` no mapa.
