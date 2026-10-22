# Offline analysis of a phase 2 session (tools/analysis/linknet_analysis.py),
# in the analysis container: ./scripts/analyze.ps1 20261020-etape4 --log captures\x.log
# Arguments through an array, as in pio.ps1.
$compose = Join-Path (Split-Path $PSScriptRoot -Parent) 'docker\docker-compose.dev.yml'
$dockerArgs = @('compose', '-f', $compose, 'run', '--rm', 'analysis', 'python', 'tools/analysis/linknet_analysis.py') + $args
& docker @dockerArgs
exit $LASTEXITCODE
