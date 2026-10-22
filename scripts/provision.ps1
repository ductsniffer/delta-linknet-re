# Writes the Wi-Fi settings into the ESP32's NVS, through the provisioning
# image (firmware/src/provision/main.cpp) on the CH343 port.
#
#   ./scripts/flash.ps1 -Port COM6 -Esptool <esptool.exe> -Target provision
#   ./scripts/provision.ps1 -Port COM6
#
# The network name and passphrase are typed here, in this window. They go to
# the board and nowhere else: not into the project, not into a log, and the
# board never echoes the passphrase. Every value travels in hex
# (lib/linknet_core/NetSettings.h), so any character is fine.
#
# The update password (flash.ps1 -Address) is generated once and kept in
# secrets\ota-password.txt, which git ignores. Run this again to change
# network: the same update password is reused.
param([Parameter(Mandatory = $true)][string]$Port)
$ErrorActionPreference = 'Stop'

function ConvertTo-Hex([string]$text) {
    [byte[]]$bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
    return -join ($bytes | ForEach-Object { $_.ToString('x2') })
}

$secrets = Join-Path (Split-Path $PSScriptRoot -Parent) 'secrets'
New-Item -ItemType Directory -Force -Path $secrets | Out-Null
$passwordFile = Join-Path $secrets 'ota-password.txt'
if (-not (Test-Path $passwordFile)) {
    # 24 characters from a 54-letter alphabet without look-alikes: ~138 bits.
    [string]$alphabet = 'abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789'
    [byte[]]$random = New-Object byte[] 24
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($random)
    [string]$generated = -join ($random | ForEach-Object { $alphabet[$_ % $alphabet.Length] })
    Set-Content -Path $passwordFile -Value $generated -NoNewline -Encoding ascii
    Write-Host "Update password created: $passwordFile"
}
[string]$otaPassword = (Get-Content -Path $passwordFile -Raw).Trim()

[string]$ssid = Read-Host 'Wi-Fi network name (2.4 GHz)'
[System.Security.SecureString]$secure = Read-Host 'Wi-Fi passphrase' -AsSecureString
[string]$passphrase = (New-Object System.Net.NetworkCredential('', $secure)).Password

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
# Released, as in console.ps1: opening the port must not reset the board.
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 500
$sp.NewLine = "`n"
$sp.Encoding = [System.Text.Encoding]::UTF8
$sp.Open()

function Send-Command([string]$line, [string]$expect, [int]$seconds) {
    $sp.Write("$line`n")
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        try {
            [string]$reply = $sp.ReadLine().TrimEnd("`r")
        } catch [System.TimeoutException] {
            continue
        }
        if ($reply -match $expect) { return $reply }
        if ($reply -match '^ERR') { throw "the ESP32 refused: $reply" }
    }
    throw "no answer to '$($line.Split(' ')[0])': is the provisioning image running on $Port?"
}

try {
    $sp.DiscardInBuffer()
    Send-Command 'show' '^SETTINGS' 5 | Out-Null
    Send-Command "ssid $(ConvertTo-Hex $ssid)" '^OK ssid' 5 | Out-Null
    Send-Command "pass $(ConvertTo-Hex $passphrase)" '^OK pass' 5 | Out-Null
    Send-Command "ota $(ConvertTo-Hex $otaPassword)" '^OK ota' 5 | Out-Null
    Write-Host (Send-Command 'save' '^SAVED' 10)
    Write-Host 'Trying the Wi-Fi (20 s at most)...'
    [string]$result = Send-Command 'test' '^WIFI (OK|FAIL)' 40
    Write-Host $result
    if ($result -match '^WIFI OK') {
        Write-Host 'Done: settings saved and Wi-Fi joined.'
    } else {
        Write-Host 'Settings saved, but the Wi-Fi refused them: check the name and passphrase, then run this script again.'
        exit 1
    }
} finally {
    $sp.Close()
}
