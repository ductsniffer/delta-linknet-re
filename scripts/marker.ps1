# Adds a marker line to the ESP32 console log that console.ps1 is recording
# (the path is in captures/SESSION_ESP32), as tools/usb/mark.py does in phase 1.
#
#   ./scripts/marker.ps1 "START test 2: thermostat 1, one press on UP"
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Text)
$ErrorActionPreference = 'Stop'
$log = (Get-Content -Path (Join-Path (Split-Path $PSScriptRoot -Parent) 'captures\SESSION_ESP32') -Raw).Trim()
$line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff') # MARKER $($Text -join ' ')`n"
[System.IO.File]::AppendAllText($log, $line, (New-Object System.Text.UTF8Encoding $false))
Write-Host $line.TrimEnd()
