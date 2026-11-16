# winboot.ps1 - poe (ou tira) o SODOS no menu do Windows Boot Manager.
#
#   powershell -ExecutionPolicy Bypass -File tools\winboot\winboot.ps1 status
#   powershell -ExecutionPolicy Bypass -File tools\winboot\winboot.ps1 install   (Administrador)
#   powershell -ExecutionPolicy Bypass -File tools\winboot\winboot.ps1 update    (Administrador)
#   powershell -ExecutionPolicy Bypass -File tools\winboot\winboot.ps1 remove    (Administrador)
#
# Ou do WSL: make winboot-install / make winboot-remove (abre o UAC).
#
# O Windows Boot Manager nao carrega um kernel Multiboot2, so um .efi. Entao:
#   - sodos-grubx64.efi (make winboot-efi) e um GRUB autonomo com o
#     tools/winboot/grub.cfg embutido: acha o FAT32 SODOSHD (o E:) e carrega
#     \BOOT\KERNEL.ELF + \BOOT\SODOS.PAK de la;
#   - install copia o .efi para \EFI\SODOS\grubx64.efi na ESP do Windows e cria
#     a entrada "SODOS" no BCD (bcdedit /copy {bootmgr} + path) no fim da ordem
#     de boot do FIRMWARE ({fwbootmgr}, variavel Boot#### na NVRAM). Nao vai no
#     menu azul do Windows: la o bootmgr carrega o .efi como aplicativo de boot
#     do Windows e recusa o GRUB (0xc000007b). A entrada padrao nao muda;
#   - next [-Restart] marca o SODOS so para o proximo boot (bootsequence do
#     firmware) e, com -Restart, reinicia;
#   - update so recopia o .efi (quando o grub.cfg muda). O kernel e os arquivos
#     do E: vao com "make sendpc", sem Administrador;
#   - remove apaga a entrada e o \EFI\SODOS.
# Antes do primeiro install o BCD vai para C:\bcd-backup-sodos-<data>
# (volta com: bcdedit /import C:\bcd-backup-sodos-<data>).
#
# Precisa do Secure Boot DESLIGADO: o .efi do GRUB nao e assinado.
# Nao e suportado oficialmente pela Microsoft, mas e o metodo comum.
param(
    [ValidateSet("status", "install", "update", "remove", "next", "fwfix", "trace")]
    [string]$Action = "status",
    [string]$Efi = "",
    [string]$Label = "SODOSHD",
    [string]$Description = "SODOS",
    [switch]$Force,
    [switch]$Restart
)

$ErrorActionPreference = "Stop"
$EfiRel = "\EFI\SODOS\grubx64.efi"
$Bcd = Join-Path $env:SystemRoot "System32\bcdedit.exe"

if (-not $Efi) { $Efi = Join-Path $PSScriptRoot "..\..\sodos-grubx64.efi" }

function Fail([string]$m) { Write-Host "Erro: $m" -ForegroundColor Red; exit 1 }
function Info([string]$m) { Write-Host $m }
function Warn([string]$m) { Write-Host "Aviso: $m" -ForegroundColor Yellow }

function Test-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-Bcd([string[]]$a) {
    $out = & $Bcd @a 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "bcdedit $($a -join ' ') falhou: $out" }
    return $out
}

# Entradas do BCD cujo path e o nosso .efi (texto do bcdedit e traduzido, entao
# procura pelo valor do path, que nao muda com o idioma).
function Get-SodosEntries {
    $txt = Invoke-Bcd @("/enum", "all", "/v")
    $ids = @()
    foreach ($blk in ($txt -split "(?:\r?\n){2,}")) {
        if ($blk -match [regex]::Escape($EfiRel)) {
            $m = [regex]::Match($blk, "\{[0-9a-fA-F-]{36}\}")
            if ($m.Success) { $ids += $m.Value }
        }
    }
    return $ids | Select-Object -Unique
}

function Get-SecureBoot {
    try { return [bool](Confirm-SecureBootUEFI) } catch { return $null }
}

# Variaveis Boot#### da NVRAM (EFI_GLOBAL_VARIABLE). O bcdedit /copy {bootmgr}
# copia o Boot#### oculto que o Windows guarda dele mesmo, entao o SODOS nasce
# com LOAD_OPTION_HIDDEN (atributo 0x9): o menu de boot da placa nao mostra e o
# firmware ASUS ignora ate o BootNext. Repair-SodosFw tira so esse bit.
$FwSrc = @"
using System;
using System.Runtime.InteropServices;
public static class SodosFw {
  const string G = "{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern uint GetFirmwareEnvironmentVariableExW(string n, string g, byte[] b, uint s, out uint a);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern bool SetFirmwareEnvironmentVariableExW(string n, string g, byte[] b, uint s, uint a);
  [DllImport("advapi32.dll", SetLastError=true)]
  static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);
  [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern bool LookupPrivilegeValue(string sys, string name, out long luid);
  [StructLayout(LayoutKind.Sequential, Pack=4)]
  struct TP { public int Count; public long Luid; public int Attr; }
  [DllImport("advapi32.dll", SetLastError=true)]
  static extern bool AdjustTokenPrivileges(IntPtr tok, bool dis, ref TP nw, int len, IntPtr prev, IntPtr rl);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  public static void Enable() {
    IntPtr t; OpenProcessToken(GetCurrentProcess(), 0x28, out t);
    TP tp = new TP(); tp.Count = 1; tp.Attr = 2;
    LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out tp.Luid);
    AdjustTokenPrivileges(t, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
  }
  public static byte[] Get(string n, out uint a) {
    byte[] b = new byte[8192];
    uint k = GetFirmwareEnvironmentVariableExW(n, G, b, (uint)b.Length, out a);
    if (k == 0) return null;
    byte[] r = new byte[k]; Array.Copy(b, r, k); return r;
  }
  public static bool Set(string n, byte[] b, uint a) {
    return SetFirmwareEnvironmentVariableExW(n, G, b, (uint)b.Length, a);
  }
}
"@

# Os Boot#### cujo caminho e o nosso .efi: @{Name; Attr (do load option)}.
function Get-SodosFw {
    if (-not ("SodosFw" -as [type])) { Add-Type -TypeDefinition $FwSrc }
    [SodosFw]::Enable()
    $r = @()
    foreach ($i in 0..0xFF) {
        $n = "Boot{0:X4}" -f $i
        $va = [uint32]0
        $b = [SodosFw]::Get($n, [ref]$va)
        if (-not $b -or $b.Length -lt 8) { continue }
        if ([Text.Encoding]::Unicode.GetString($b).IndexOf($EfiRel, [StringComparison]::OrdinalIgnoreCase) -lt 0) { continue }
        $r += [pscustomobject]@{ Name = $n; Attr = [BitConverter]::ToUInt32($b, 0); VarAttr = $va; Data = $b }
    }
    return $r
}

function Repair-SodosFw {
    $list = @(Get-SodosFw)
    if ($list.Count -eq 0) { Warn "nenhum Boot#### com $EfiRel na NVRAM."; return }
    foreach ($e in $list) {
        if (($e.Attr -band 9) -eq 1) { Info ("NVRAM:         {0} visivel (attr=0x{1:X})" -f $e.Name, $e.Attr); continue }
        $na = [uint32]$e.Attr
        if ($na -band 8) { $na = $na - 8 }
        $na = $na -bor 1
        $d = $e.Data
        [BitConverter]::GetBytes([uint32]$na).CopyTo($d, 0)
        if (-not [SodosFw]::Set($e.Name, $d, $e.VarAttr)) { Fail ("nao consegui gravar {0} na NVRAM." -f $e.Name) }
        $chk = @(Get-SodosFw | Where-Object { $_.Name -eq $e.Name })
        if ($chk.Count -eq 0 -or ($chk[0].Attr -band 9) -ne 1) { Fail ("{0} gravado, mas a leitura nao confere." -f $e.Name) }
        Info ("NVRAM:         {0} attr 0x{1:X} -> 0x{2:X} (ativa, sem oculta)" -f $e.Name, $e.Attr, $chk[0].Attr)
    }
}

# Monta a ESP (a particao de sistema de onde o Windows deu boot) numa letra livre.
function Mount-Esp {
    $used = (Get-PSDrive -PSProvider FileSystem).Name
    $free = [char[]]"ZYXWVUTSRQPONMLKJIH" | Where-Object { $used -notcontains [string]$_ } | Select-Object -First 1
    if (-not $free) { Fail "nenhuma letra livre para montar a ESP." }
    $L = "$($free):"
    & mountvol $L /s | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$L\EFI")) { Fail "mountvol $L /s nao montou a ESP." }
    return $L
}
function Dismount-Esp([string]$L) { if ($L) { & mountvol $L /d | Out-Null } }

function Show-Status {
    $sb = Get-SecureBoot
    Info ("Firmware:      " + $env:firmware_type)
    if ($sb -eq $null) { Info "Secure Boot:   ? (rode como Administrador para ver)" }
    elseif ($sb) { Warn "Secure Boot LIGADO: desligue no setup do firmware, senao o SODOS nao carrega." }
    else { Info "Secure Boot:   desligado (ok)" }

    $v = Get-Volume | Where-Object { $_.FileSystemLabel -eq $Label -and $_.FileSystem -eq "FAT32" }
    if (-not $v) { Warn "nenhum volume FAT32 com rotulo $Label." }
    foreach ($x in $v) {
        $d = if ($x.DriveLetter) { "$($x.DriveLetter):" } else { "(sem letra)" }
        $k = if ($x.DriveLetter) { Test-Path "$($x.DriveLetter):\BOOT\KERNEL.ELF" } else { $false }
        $p = $x | Get-Partition
        $disk = Get-Disk -Number $p.DiskNumber
        Info ("Particao:      $d $Label no disco $($p.DiskNumber) ($($disk.FriendlyName), $($disk.BusType)), " +
              ("{0:N1} GB" -f ($x.Size / 1GB)))
        if ($k) { Info "Kernel:        $d\BOOT\KERNEL.ELF ok" }
        else { Warn "$d\BOOT\KERNEL.ELF nao existe: rode 'make sendpc' no WSL." }
        if ($disk.BusType -ne "NVMe") { Warn "o SODOS so acha o $Label em SSD NVMe (kernel/nvme.c); este disco e $($disk.BusType)." }
    }
    if ($v -and @($v).Count -gt 1) { Warn "mais de um volume ${Label}: o GRUB e o kernel usam o primeiro que acharem." }

    if (Test-Path $Efi) { Info "GRUB EFI:      $((Resolve-Path $Efi).Path)" }
    else { Warn "$Efi nao existe: rode 'make winboot-efi' no WSL." }

    if (Test-Admin) {
        $ids = Get-SodosEntries
        if ($ids) {
            Info "Entrada BCD:   $($ids -join ', ')"
            $fw = Invoke-Bcd @("/enum", "{fwbootmgr}", "/v")
            if ($fw -match [regex]::Escape(@($ids)[0])) { Info "Firmware:      na ordem de boot do firmware (ok)" }
            else { Warn "a entrada nao esta na ordem do firmware: rode install de novo." }
            foreach ($e in @(Get-SodosFw)) {
                if (($e.Attr -band 9) -eq 1) { Info ("NVRAM:         {0} visivel (attr=0x{1:X})" -f $e.Name, $e.Attr) }
                else { Warn ("{0} na NVRAM com attr=0x{1:X} (oculta ou inativa): rode fwfix." -f $e.Name, $e.Attr) }
            }
            $mgr0 = Invoke-Bcd @("/enum", "{bootmgr}", "/v")
            if ($mgr0 -match [regex]::Escape(@($ids)[0])) { Warn "a entrada ainda esta no menu do Windows (da 0xc000007b): rode install de novo." }
        } else { Info "Entrada BCD:   nenhuma (rode install)" }
        $L = Mount-Esp
        try {
            if (Test-Path "$L$EfiRel") {
                $same = (Test-Path $Efi) -and ((Get-FileHash "$L$EfiRel").Hash -eq (Get-FileHash $Efi).Hash)
                Info ("Na ESP:        $EfiRel " + $(if ($same) { "(igual ao do projeto)" } else { "(diferente: rode update)" }))
            } else { Info "Na ESP:        $EfiRel nao existe" }
        } finally { Dismount-Esp $L }
        $mgr = Invoke-Bcd @("/enum", "{bootmgr}")
        $t = [regex]::Match($mgr, "(?m)^timeout\s+(\d+)")
        if ($t.Success -and [int]$t.Groups[1].Value -eq 0) { Warn "timeout do menu = 0: o menu nao aparece (bcdedit /timeout 10)." }
    } else {
        Info "(BCD e ESP: rode como Administrador para ver)"
    }
}

function Copy-Efi([string]$L) {
    $dir = "$L\EFI\SODOS"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item -Force $Efi "$L$EfiRel"
    Info "Copiado: $EfiRel na ESP."
    Reset-Trace "$dir\GRUBENV"
}

# Rastro do GRUB: bloco de ambiente de 1024 bytes (formato do grub-editenv).
# O GRUB grava direto nos setores do arquivo (a data do arquivo nao muda), entao
# o next zera os dois antes de reiniciar: o que estiver neles e do ultimo boot.
function Reset-Trace([string]$p) {
    $h = [Text.Encoding]::ASCII.GetBytes("# GRUB Environment Block`n")
    $b = New-Object byte[] 1024
    for ($i = 0; $i -lt 1024; $i++) { $b[$i] = 35 }
    [Array]::Copy($h, $b, $h.Length)
    # Regrava no lugar (mesmos clusters), sem truncar.
    $f = [IO.File]::Open($p, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::Write)
    try { $f.Write($b, 0, 1024); $f.SetLength(1024) } finally { $f.Close() }
}
function Read-Trace([string]$p) {
    if (-not (Test-Path $p)) { return "(sem $p)" }
    $t = [IO.File]::ReadAllText($p) -split "`n" | Where-Object { $_ -match "=" }
    if ($t) { return ($t -join "; ") } else { return "(vazio: o GRUB nao gravou nada)" }
}
function Get-DataTrace {
    $v = Get-Volume | Where-Object { $_.FileSystemLabel -eq $Label -and $_.FileSystem -eq "FAT32" -and $_.DriveLetter } | Select-Object -First 1
    if ($v) { return "$($v.DriveLetter):\BOOT\GRUBENV" } else { return $null }
}
function Show-Trace([string]$L) {
    Info ("Rastro ESP:    " + (Read-Trace "$L\EFI\SODOS\GRUBENV") + "   (etapa 0 = o firmware abriu o GRUB)")
    $d = Get-DataTrace
    if ($d) { Info ("Rastro $($d.Substring(0,2))     " + (Read-Trace $d) + "   (1 achou E:, 1b kernel, 1c SODOS.PAK, 2 boot)") }
}

if ($Action -eq "status") { Show-Status; exit 0 }

if (-not (Test-Admin)) { Fail "rode como Administrador (ou use 'make winboot-install' no WSL)." }
if ($env:firmware_type -ne "UEFI") { Fail "este Windows nao deu boot em UEFI ($env:firmware_type)." }

switch ($Action) {
    "install" {
        if (-not (Test-Path $Efi)) { Fail "$Efi nao existe: rode 'make winboot-efi' no WSL." }
        $sb = Get-SecureBoot
        if ($sb -and -not $Force) {
            Fail "Secure Boot ligado: o Windows Boot Manager recusa o GRUB nao assinado. Desligue no firmware (ou -Force)."
        }
        $bak = Join-Path $env:SystemDrive ("bcd-backup-sodos-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
        Invoke-Bcd @("/export", $bak) | Out-Null
        Info "Backup do BCD: $bak"

        $L = Mount-Esp
        try {
            $free = $null
            try { $free = (New-Object System.IO.DriveInfo($L)).AvailableFreeSpace } catch {}
            if ($free -ne $null -and $free -lt ((Get-Item $Efi).Length + 1MB)) { Fail "ESP sem espaco ($free bytes livres)." }
            Copy-Efi $L
        } finally { Dismount-Esp $L }

        $ids = @(Get-SodosEntries)
        if ($ids.Count -gt 0) {
            $id = $ids[0]
            Info "Entrada ja existia: $id"
        } else {
            $out = Invoke-Bcd @("/copy", "{bootmgr}", "/d", $Description)
            $m = [regex]::Match($out, "\{[0-9a-fA-F-]{36}\}")
            if (-not $m.Success) { Fail "nao achei o GUID na saida do bcdedit: $out" }
            $id = $m.Value
            Invoke-Bcd @("/set", $id, "path", $EfiRel) | Out-Null
            Info "Entrada criada: $id"
        }
        Invoke-Bcd @("/set", $id, "description", $Description) | Out-Null
        # Fora do menu do Windows: o bootmgr carrega a entrada como aplicativo de
        # boot do Windows e recusa o GRUB (0xc000007b). Vai para o menu do
        # firmware ({fwbootmgr} = variavel Boot#### na NVRAM), no fim da ordem.
        & $Bcd /displayorder $id /remove 2>&1 | Out-Null
        Invoke-Bcd @("/set", "{fwbootmgr}", "displayorder", $id, "/addlast") | Out-Null
        Repair-SodosFw
        Info ""
        Info "Pronto. A entrada '$Description' esta no menu de boot do firmware (tecla de boot da placa, F8/F11/F12)."
        Info "Para reiniciar direto no SODOS uma vez: winboot.ps1 next -Restart (ou make winboot-next)."
        Show-Status
    }
    "next" {
        $ids = @(Get-SodosEntries)
        if ($ids.Count -eq 0) { Fail "entrada SODOS nao existe: rode install." }
        Repair-SodosFw   # o Windows pode ter posto a marca de oculta de novo
        $L = Mount-Esp
        try {
            Info "Rastro do boot anterior:"
            Show-Trace $L
            if (Test-Path "$L\EFI\SODOS") { Reset-Trace "$L\EFI\SODOS\GRUBENV" }
        } finally { Dismount-Esp $L }
        $d = Get-DataTrace
        if ($d) { Reset-Trace $d }
        Info "Rastros zerados para este teste."
        Invoke-Bcd @("/set", "{fwbootmgr}", "bootsequence", $ids[0]) | Out-Null
        Info "Proximo boot (so uma vez): $Description. Depois volta ao Windows."
        if ($Restart) { & shutdown.exe /r /t 3 /c "Reiniciando no SODOS" }
        else { Info "Reinicie quando quiser." }
    }
    "fwfix" {
        Repair-SodosFw
    }
    "trace" {
        $L = Mount-Esp
        try { Show-Trace $L } finally { Dismount-Esp $L }
    }
    "update" {
        if (-not (Test-Path $Efi)) { Fail "$Efi nao existe: rode 'make winboot-efi' no WSL." }
        $L = Mount-Esp
        try { Copy-Efi $L } finally { Dismount-Esp $L }
    }
    "remove" {
        foreach ($id in @(Get-SodosEntries)) {
            & $Bcd /set "{fwbootmgr}" displayorder $id /remove 2>&1 | Out-Null
            & $Bcd /displayorder $id /remove 2>&1 | Out-Null
            Invoke-Bcd @("/delete", $id) | Out-Null
            Info "Entrada apagada: $id"
        }
        $L = Mount-Esp
        try {
            if (Test-Path "$L\EFI\SODOS") { Remove-Item -Recurse -Force "$L\EFI\SODOS"; Info "Apagado: \EFI\SODOS da ESP." }
        } finally { Dismount-Esp $L }
    }
}
