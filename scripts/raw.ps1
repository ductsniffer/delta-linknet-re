# Records the board's raw stream: every byte of the bus with its time in
# microseconds (tcp/24, firmware/lib/linknet_core/RawCapture.h). This is the
# material for the analysis: timing histograms, a replay through the
# decoder, a .pcap. console.ps1 records the decoded lines beside it.
#
#   ./scripts/raw.ps1 -Address 192.168.1.50 -Session 20261020-etape4
#
# One file per connection, captures/esp32-<session>-raw-<n>.raw:
#   line 1  "HOST <host time when connected>"
#   line 2  the board's header: "LNKRAW1 image=... now_us=<board clock> ..."
#   then    6-byte records: u32 LE time (us, low 32 bits), u8 byte, u8 flags
#           (bit 0: bytes lost just before this one, the board's ring was full)
# The two first lines tie the board's clock to the host's, give or take the
# Wi-Fi latency. A new connection gets a new file, so a file never holds two
# streams spliced together.
#
# Reconnects by itself, like console.ps1; 30 s without data means the
# connection is dead (the bus sends ~1.3 kB/s).
param(
    [Parameter(Mandatory = $true)][string]$Address,
    [int]$TcpPort = 24,
    [string]$Session = (Get-Date -Format 'yyyyMMdd-HHmmss')
)
$ErrorActionPreference = 'Stop'
$captures = Join-Path (Split-Path $PSScriptRoot -Parent) 'captures'
[int]$n = 0
[bool]$failing = $false
Write-Host "recording ${Address}:$TcpPort to $captures\esp32-$Session-raw-<n>.raw"
while ($true) {
    [System.Net.Sockets.TcpClient]$tcp = New-Object System.Net.Sockets.TcpClient
    [System.IO.FileStream]$file = $null
    try {
        $tcp.Connect($Address, $TcpPort)
        $tcp.ReceiveTimeout = 30000
        $failing = $false
        # Next free number: a restarted script must not reuse a file.
        do {
            $n += 1
            [string]$path = Join-Path $captures ("esp32-$Session-raw-{0:d3}.raw" -f $n)
        } while (Test-Path $path)
        # Shared for reading: the analysis may look at it while it grows.
        $file = [System.IO.File]::Open($path, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
        [byte[]]$stamp = [System.Text.Encoding]::ASCII.GetBytes("HOST $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ss.fffzzz')`n")
        $file.Write($stamp, 0, $stamp.Length)
        Write-Host "$(Get-Date -Format 'HH:mm:ss') connected: $path"
        [System.Net.Sockets.NetworkStream]$stream = $tcp.GetStream()
        [byte[]]$buf = New-Object byte[] 8192
        while ($true) {
            [int]$got = $stream.Read($buf, 0, $buf.Length)
            if ($got -le 0) { throw 'closed by the board' }
            $file.Write($buf, 0, $got)
            $file.Flush()
        }
    } catch {
        if ($file) {
            Write-Host "$(Get-Date -Format 'HH:mm:ss') disconnected: $($_.Exception.Message)"
        } elseif (-not $failing) {
            Write-Host "$(Get-Date -Format 'HH:mm:ss') unreachable: $($_.Exception.Message)"
            $failing = $true
        }
    } finally {
        if ($file) { $file.Close() }
        $tcp.Close()
    }
    Start-Sleep -Seconds 5
}
