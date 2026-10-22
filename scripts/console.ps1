# The ESP32's console, recorded: each line, stamped with the host clock, is
# appended to captures/esp32-<session>.log. marker.ps1 adds test markers to
# the same file, as tools/usb/capture.py and mark.py do in phase 1.
#
#   ./scripts/console.ps1 -Port COM6                  # over the cable: the CH343 port (UART0, NOT the bus)
#   ./scripts/console.ps1 -Port COM6 -Reset           # restart the board first, to see its boot banner
#   ./scripts/console.ps1 -Address 192.168.1.50     # over the Wi-Fi: tcp/23 (lib/linknet_esp32/Esp32Net.h)
#   Get-Content -Wait captures\esp32-<session>.log      # to watch it
#
# Serial: uses the port PuTTY would; close PuTTY first, and stop this before
# flashing. DTR and RTS stay released, so opening the port does not reset
# the board (they drive EN and GPIO0 through the auto-program circuit).
# -Reset asserts RTS alone for 100 ms, which pulls EN low with GPIO0 high: a
# normal reboot, the same "hard reset" esptool does after flashing.
#
# Wi-Fi: reconnects by itself, after a reboot of the board, a Wi-Fi drop, or
# 150 s without a line (the board writes at least once a minute). The board
# queues what it logs while nobody is connected, so its lines arrive late
# rather than not at all; those carry the board's own uptime, the host stamp
# is then the time of arrival.
[CmdletBinding(DefaultParameterSetName = 'serial')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'serial')][string]$Port,
    [Parameter(ParameterSetName = 'serial')][switch]$Reset,
    [Parameter(Mandatory = $true, ParameterSetName = 'tcp')][string]$Address,
    [Parameter(ParameterSetName = 'tcp')][int]$TcpPort = 23,
    [string]$Session = (Get-Date -Format 'yyyyMMdd-HHmmss')
)
$ErrorActionPreference = 'Stop'
$captures = Join-Path (Split-Path $PSScriptRoot -Parent) 'captures'
$log = Join-Path $captures "esp32-$Session.log"
Set-Content -Path (Join-Path $captures 'SESSION_ESP32') -Value $log -Encoding utf8
$utf8 = New-Object System.Text.UTF8Encoding $false

function Get-Stamp { return (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff') }

function Write-LogLine([string]$text) {
    # Opened and closed per line: marker.ps1 appends to the same file, and
    # a stream held open here would write over its lines.
    [System.IO.File]::AppendAllText($log, "$text`n", $utf8)
}

if ($PSCmdlet.ParameterSetName -eq 'tcp') {
    Write-LogLine "$(Get-Stamp) # START tcp=${Address}:$TcpPort"
    Write-Host "recording ${Address}:$TcpPort to $log"
    [bool]$failing = $false
    while ($true) {
        [System.Net.Sockets.TcpClient]$tcp = New-Object System.Net.Sockets.TcpClient
        [bool]$connected = $false
        try {
            $tcp.Connect($Address, $TcpPort)
            $tcp.ReceiveTimeout = 150000
            $connected = $true
            $failing = $false
            Write-LogLine "$(Get-Stamp) # CONNECTED ${Address}:$TcpPort"
            [System.IO.StreamReader]$reader = New-Object System.IO.StreamReader($tcp.GetStream(), [System.Text.Encoding]::ASCII)
            while ($true) {
                # Not [string]$line: that turns the $null of end-of-stream
                # into "", and a closed connection into endless empty lines
                # (bench, 2026-10-17).
                $line = $reader.ReadLine()
                if ($null -eq $line) { throw 'closed by the board' }
                Write-LogLine "$(Get-Stamp) $($line.TrimEnd("`r"))"
            }
        } catch {
            # Once per outage, not once per retry.
            if ($connected) {
                Write-LogLine "$(Get-Stamp) # DISCONNECTED $($_.Exception.Message)"
            } elseif (-not $failing) {
                Write-LogLine "$(Get-Stamp) # UNREACHABLE $($_.Exception.Message)"
                $failing = $true
            }
        } finally {
            $tcp.Close()
        }
        Start-Sleep -Seconds 5
    }
}

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 1000
$sp.NewLine = "`n"
$sp.Encoding = [System.Text.Encoding]::ASCII
$sp.Open()
Write-LogLine "$(Get-Stamp) # START port=$Port 115200 8N1"
if ($Reset) {
    $sp.RtsEnable = $true
    Start-Sleep -Milliseconds 100
    $sp.RtsEnable = $false
    Write-LogLine "$(Get-Stamp) # RESET (RTS pulse)"
}
Write-Host "recording $Port to $log"
try {
    while ($true) {
        try {
            [string]$line = $sp.ReadLine()
        } catch [System.TimeoutException] {
            continue
        }
        Write-LogLine "$(Get-Stamp) $($line.TrimEnd("`r"))"
    }
} finally {
    $sp.Close()
    Write-LogLine "$(Get-Stamp) # END"
}
