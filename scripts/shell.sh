#!/bin/sh
exec docker compose -f "$(dirname -- "$0")/../docker/docker-compose.dev.yml" run --rm dev bash "$@"
