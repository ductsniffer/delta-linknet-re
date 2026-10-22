# Run a PlatformIO command inside the dev container (working directory firmware/).
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
docker compose -f "$PSScriptRoot\..\docker\docker-compose.dev.yml" run --rm dev pio @Rest
exit $LASTEXITCODE
