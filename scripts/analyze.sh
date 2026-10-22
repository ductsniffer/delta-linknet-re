#!/bin/sh
# Offline analysis of a phase 2 session (tools/analysis/linknet_analysis.py),
# in the analysis container: ./scripts/analyze.sh 20261020-etape4 [--log captures/x.log]
exec docker compose -f "$(dirname "$0")/../docker/docker-compose.dev.yml" run --rm analysis \
    python tools/analysis/linknet_analysis.py "$@"
