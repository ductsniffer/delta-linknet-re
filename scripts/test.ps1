# Unit tests and the capture replay, on the host compiler inside the container.
# No board needed: lib_ignore drops linknet_esp32 from the native build.
# The arguments go through an array on purpose: written inline, PowerShell
# reads "-e" as an abbreviation of its own -ErrorAction and refuses it.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$pioArgs = @('test', '-e', 'native') + @($Rest | Where-Object { $_ -ne $null })
& "$PSScriptRoot\pio.ps1" @pioArgs
exit $LASTEXITCODE
