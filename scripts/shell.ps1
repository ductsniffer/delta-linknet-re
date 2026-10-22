docker compose -f "$PSScriptRoot\..\docker\docker-compose.dev.yml" run --rm dev bash
exit $LASTEXITCODE
