#!/bin/sh
# Unit tests and the capture replay, on the host compiler inside the container.
exec "$(dirname -- "$0")/pio.sh" test -e native "$@"
