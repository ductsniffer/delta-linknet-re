# Compile the ESP32-S3 listener firmware. Catches Arduino API misuse without a board.
# The arguments go through an array on purpose: written inline, PowerShell
# reads "-e" as an abbreviation of its own -ErrorAction and refuses it.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$pioArgs = @('run', '-e', 'esp32s3') + @($Rest | Where-Object { $_ -ne $null })
& "$PSScriptRoot\pio.ps1" @pioArgs
exit $LASTEXITCODE
