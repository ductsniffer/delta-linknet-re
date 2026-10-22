#!/bin/sh
# Run a PlatformIO command inside the dev container (working directory firmware/).
exec docker compose -f "$(dirname -- "$0")/../docker/docker-compose.dev.yml" run --rm dev pio "$@"
