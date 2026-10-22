# Build a firmware in the container, copy its images out of the pio-build
# volume, and install it: over the USB cable with esptool.exe, or over the
# Wi-Fi with curl.exe (part of Windows).
#
#   ./scripts/flash.ps1 -Port COM6 -Esptool C:\path\to\esptool.exe                    # the listener (esp32s3)
#   ./scripts/flash.ps1 -Port COM6 -Esptool C:\path\to\esptool.exe -Target edges      # the wiring-check edge counter
#   ./scripts/flash.ps1 -Port COM6 -Esptool C:\path\to\esptool.exe -Target provision  # Wi-Fi settings, then provision.ps1
#   ./scripts/flash.ps1 -Address 192.168.1.50 [-Target edges]                        # over the Wi-Fi
#
# USB: -Port is the CH343 one (UART0, with the automatic reset), not the
# native USB port. Close PuTTY or console.ps1 -Port first: they hold the port.
# Addresses and flash settings are the ones PlatformIO itself uses for this
# board (pio run -e esp32s3 -t upload -v, 2026-10-17):
#   0x0000 bootloader, 0x8000 partitions, 0xe000 boot_app0, 0x10000 firmware;
#   --flash_mode dio --flash_freq 80m --flash_size 8MB.
#
# Wi-Fi: POST /update on the board (lib/linknet_esp32/Esp32Net.h), with the
# password provision.ps1 left in secrets\ota-password.txt. The board checks
# the image, restarts into it, and confirms it once it is back online; until
# then a failure sends it back to the previous image by itself. This script
# waits for that confirmation.
[CmdletBinding(DefaultParameterSetName = 'usb')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'usb')][string]$Port,
    [Parameter(Mandatory = $true, ParameterSetName = 'usb')][string]$Esptool,
    [Parameter(ParameterSetName = 'usb')][int]$Baud = 921600,
    [Parameter(Mandatory = $true, ParameterSetName = 'wifi')][string]$Address,
    [ValidateSet('esp32s3', 'edges', 'provision')][string]$Target = 'esp32s3'
)
$ErrorActionPreference = 'Stop'
$compose = Join-Path (Split-Path $PSScriptRoot -Parent) 'docker\docker-compose.dev.yml'
$dist = Join-Path (Split-Path $PSScriptRoot -Parent) "firmware\dist\$Target"

if ($Address -and $Target -eq 'provision') {
    # It has no Wi-Fi: installed this way, the board would be out of reach.
    throw 'the provisioning image goes over USB only'
}

# 1. Build. Arguments through an array: inline, PowerShell would read -e
#    as its own -ErrorAction.
$pioArgs = @('run', '-e', $Target)
& (Join-Path $PSScriptRoot 'pio.ps1') @pioArgs
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

# 2. The images live in the pio-build volume, invisible from Windows.
$copy = "mkdir -p /work/firmware/dist/$Target && cp /build/pio/$Target/bootloader.bin /build/pio/$Target/partitions.bin /build/pio/$Target/firmware.bin /pio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin /work/firmware/dist/$Target/"
docker compose -f $compose run --rm --no-deps dev sh -c $copy
if ($LASTEXITCODE -ne 0) { throw "copy failed ($LASTEXITCODE)" }

if ($PSCmdlet.ParameterSetName -eq 'wifi') {
    $passwordFile = Join-Path (Split-Path $PSScriptRoot -Parent) 'secrets\ota-password.txt'
    if (-not (Test-Path $passwordFile)) { throw "no ${passwordFile}, run provision.ps1 first" }
    [string]$password = (Get-Content -Path $passwordFile -Raw).Trim()
    [string]$image = Join-Path $dist 'firmware.bin'
    [string]$base = "http://$Address"

    function Get-BoardStatus {
        [string]$text = (& curl.exe --silent --max-time 4 "$base/" | Out-String)
        if ($LASTEXITCODE -ne 0) { return $null }
        return $text.Trim()
    }

    [string]$before = Get-BoardStatus
    if (-not $before) { throw "$base does not answer: is the board on the Wi-Fi?" }
    Write-Host "before: $(($before -split "`n")[0])"
    Write-Host "sending $image ($((Get-Item $image).Length) bytes) to $base/update"
    & curl.exe --silent --show-error --fail-with-body --max-time 180 -u "linknet:$password" `
        -F "firmware=@$image;type=application/octet-stream" "$base/update"
    if ($LASTEXITCODE -ne 0) { throw "update refused or interrupted (curl $LASTEXITCODE); the board keeps its image" }

    # Back online and confirmed: Esp32Net.cpp confirms after 10 s online.
    $deadline = (Get-Date).AddSeconds(120)
    [string]$after = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        $after = Get-BoardStatus
        if ($after -and $after -match '\((confirmed|flashed over USB)\)') { break }
    }
    if (-not $after) { throw 'the board did not come back within 2 minutes: watch its LED (blinking = no Wi-Fi)' }
    Write-Host $after
    if ($after -notmatch '\(confirmed\)') {
        Write-Warning 'not confirmed yet: if it never gets online, it goes back to the previous image by itself'
        exit 1
    }
    exit 0
}

# 3. esptool 5 renamed write_flash to write-flash and the options to dashes.
$versionText = (& $Esptool version 2>&1 | Out-String)
[bool]$v5 = $versionText -match '(?m)\bv?(\d+)\.\d+' -and [int]$Matches[1] -ge 5
Write-Host "esptool: $($versionText.Trim()) -> $(if ($v5) { 'write-flash' } else { 'write_flash' })"

$images = @(
    '0x0', (Join-Path $dist 'bootloader.bin'),
    '0x8000', (Join-Path $dist 'partitions.bin'),
    '0xe000', (Join-Path $dist 'boot_app0.bin'),
    '0x10000', (Join-Path $dist 'firmware.bin')
)
if ($v5) {
    $flashArgs = @('--chip', 'esp32s3', '--port', $Port, '--baud', "$Baud",
        '--before', 'default-reset', '--after', 'hard-reset',
        'write-flash', '-z', '--flash-mode', 'dio', '--flash-freq', '80m', '--flash-size', '8MB') + $images
} else {
    $flashArgs = @('--chip', 'esp32s3', '--port', $Port, '--baud', "$Baud",
        '--before', 'default_reset', '--after', 'hard_reset',
        'write_flash', '-z', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '8MB') + $images
}
Write-Host "$Esptool $($flashArgs -join ' ')"
& $Esptool @flashArgs
exit $LASTEXITCODE
