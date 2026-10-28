param(
    [string]$VmName = "SODOS",
    [string]$ProjectDir = "",
    [switch]$Start,
    [switch]$RefreshDisks
)

$ErrorActionPreference = "Continue"

function Fail([string]$Message) {
    Write-Host "Erro: $Message" -ForegroundColor Red
    exit 1
}

function Invoke-VBox {
    param([string[]]$Arguments)
    & $script:VBoxManage @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "VBoxManage falhou: $($Arguments -join ' ')"
    }
}

function Detach-Disk([int]$Port, [int]$Device) {
    & $VBoxManage storageattach $VmName --storagectl IDE --port $Port --device $Device --type hdd --medium none 2>$null | Out-Null
}

function Remove-Medium([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        & $VBoxManage closemedium disk $Path --delete 2>$null | Out-Null
        if (Test-Path -LiteralPath $Path) {
            Remove-Item -LiteralPath $Path -Force
        }
    }
}

function New-Vdi([string]$Raw, [string]$Vdi, [int]$PadBytes) {
    $source = $Raw
    if ($PadBytes -gt 0) {
        $bytes = [IO.File]::ReadAllBytes($Raw)
        if ($bytes.Length -gt $PadBytes) {
            throw "Imagem $Raw maior que o padding de $PadBytes bytes."
        }
        $buf = New-Object byte[] $PadBytes
        [Array]::Copy($bytes, $buf, $bytes.Length)
        $source = Join-Path $vmDir ("vbox-" + [IO.Path]::GetFileNameWithoutExtension($Vdi) + ".img")
        [IO.File]::WriteAllBytes($source, $buf)
    }
    Invoke-VBox -Arguments @("convertfromraw", $source, $Vdi, "--format", "VDI")
    if ($PadBytes -gt 0 -and (Test-Path -LiteralPath $source)) {
        Remove-Item -LiteralPath $source -Force
    }
}

function Ensure-Disk([string]$Raw, [string]$Vdi, [int]$Port, [int]$Device) {
    $needCreate = $RefreshDisks -or -not (Test-Path -LiteralPath $Vdi)
    Detach-Disk $Port $Device
    if ($needCreate) {
        Remove-Medium $Vdi
        New-Vdi $Raw $Vdi 0
    }
    Invoke-VBox -Arguments @(
        "storageattach", $VmName,
        "--storagectl", "IDE",
        "--port", $Port,
        "--device", $Device,
        "--type", "hdd",
        "--medium", $Vdi
    )
}

$candidates = @()
if ($env:ProgramFiles) {
    $candidates += (Join-Path $env:ProgramFiles "Oracle\VirtualBox\VBoxManage.exe")
}
$pf86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
if ($pf86) {
    $candidates += (Join-Path $pf86 "Oracle\VirtualBox\VBoxManage.exe")
}
$VBoxManage = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $VBoxManage) {
    $cmd = Get-Command VBoxManage.exe -ErrorAction SilentlyContinue
    if ($cmd) { $VBoxManage = $cmd.Source }
}
if (-not $VBoxManage) {
    Fail "VBoxManage.exe nao encontrado. Instale o VirtualBox ou adicione ao PATH."
}

if ([string]::IsNullOrWhiteSpace($ProjectDir)) {
    $ProjectDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
$osImg = Join-Path $ProjectDir "os.img"
$diskImg = Join-Path $ProjectDir "disk.img"
$disk2Img = Join-Path $ProjectDir "disk2.img"
foreach ($img in @($osImg, $diskImg, $disk2Img)) {
    if (-not (Test-Path -LiteralPath $img)) {
        Fail "Imagem ausente: $img. Rode 'wsl make os.img disk.img disk2.img' primeiro."
    }
}

$info = & $VBoxManage showvminfo $VmName --machinereadable 2>$null
$exists = ($LASTEXITCODE -eq 0)
if (-not $exists) {
    Write-Host "Criando VM $VmName..."
    Invoke-VBox -Arguments @("createvm", "--name", $VmName, "--ostype", "Other_64", "--register")
    $info = & $VBoxManage showvminfo $VmName --machinereadable
}

$stateLine = $info | Where-Object { $_ -match '^VMState="(.+)"' } | Select-Object -First 1
if (-not $stateLine) { Fail "Nao foi possivel ler o estado da VM $VmName." }
$state = [regex]::Match($stateLine, '^VMState="(.+)"').Groups[1].Value
if ($state -eq "running" -or $state -eq "paused") {
    Write-Host "Desligando a VM $VmName para atualizar os discos..."
    Invoke-VBox -Arguments @("controlvm", $VmName, "poweroff")
    Start-Sleep -Seconds 2
} elseif ($state -eq "saved") {
    Write-Host "Descartando o estado salvo da VM $VmName..."
    Invoke-VBox -Arguments @("discardstate", $VmName)
} elseif ($state -ne "poweroff" -and $state -ne "aborted") {
    Fail "Estado da VM nao suportado: $state"
}

$info = & $VBoxManage showvminfo $VmName --machinereadable
$cfgLine = $info | Where-Object { $_ -match '^CfgFile="(.+)"' } | Select-Object -First 1
if (-not $cfgLine) { Fail "Nao foi possivel localizar o arquivo .vbox da VM $VmName." }
$cfgFile = [regex]::Match($cfgLine, '^CfgFile="(.+)"').Groups[1].Value
$cfgFile = $cfgFile.Replace('\\', '\')
$vmDir = Split-Path -Parent $cfgFile
$serial = Join-Path $vmDir "serial.log"

Write-Host "Configurando VM $VmName (BIOS, 256 MB, VBoxSVGA)..."
Invoke-VBox -Arguments @(
    "modifyvm", $VmName,
    "--memory", "256",
    "--cpus", "1",
    "--firmware", "bios",
    "--boot1", "disk",
    "--boot2", "none",
    "--boot3", "none",
    "--boot4", "none",
    "--graphicscontroller", "vboxsvga",
    "--vram", "16",
    "--accelerate3d", "off",
    "--audio-driver", "none",
    "--usb", "off",
    "--nic1", "none",
    "--ioapic", "on",
    "--pae", "on",
    "--clipboard", "disabled",
    "--draganddrop", "disabled",
    "--uart1", "0x3F8", "4",
    "--uart-type1", "16550A",
    "--uartmode1", "file", $serial
)

$info = & $VBoxManage showvminfo $VmName --machinereadable
if (-not ($info -match '^storagecontrollername\d+="IDE"')) {
    Invoke-VBox -Arguments @("storagectl", $VmName, "--name", "IDE", "--add", "ide", "--controller", "PIIX4")
}

if ($RefreshDisks) {
    Write-Host "AVISO: -RefreshDisks recria disk.vdi/disk2.vdi a partir de disk.img/disk2.img;"
    Write-Host "       alteracoes feitas dentro da VM em D: ou no disk2 serao perdidas."
}

$osVdi = Join-Path $vmDir "os.vdi"
$diskVdi = Join-Path $vmDir "disk.vdi"
$disk2Vdi = Join-Path $vmDir "disk2.vdi"

$osLen = (Get-Item -LiteralPath $osImg).Length
$osPad = [int]([math]::Ceiling(($osLen + 1MB) / 1MB) * 1MB)
if ($osPad -lt 2MB) { $osPad = 2MB }

Write-Host "Atualizando os.vdi (os.img=$osLen bytes, padding=$osPad)..."
Detach-Disk 0 0
Remove-Medium $osVdi
New-Vdi $osImg $osVdi $osPad
Invoke-VBox -Arguments @(
    "storageattach", $VmName,
    "--storagectl", "IDE",
    "--port", "0",
    "--device", "0",
    "--type", "hdd",
    "--medium", $osVdi
)

Write-Host "Anexando disk.vdi (D:)..."
Ensure-Disk $diskImg $diskVdi 0 1
Write-Host "Anexando disk2.vdi..."
Ensure-Disk $disk2Img $disk2Vdi 1 0

Write-Host "VM $VmName pronta em $vmDir"
if ($Start) {
    Write-Host "Iniciando $VmName..."
    Invoke-VBox -Arguments @("startvm", $VmName, "--type", "gui")
} else {
    Write-Host "Inicie pela interface do VirtualBox ou com:"
    Write-Host "  VBoxManage startvm $VmName --type gui"
}
exit 0
