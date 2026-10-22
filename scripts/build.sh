#!/bin/sh
# Compile the ESP32-S3 listener firmware. Catches Arduino API misuse without a board.
exec "$(dirname -- "$0")/pio.sh" run -e esp32s3 "$@"
