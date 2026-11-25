#!/bin/bash
# Shared entrypoint for the pli test images.
# No arguments + a TTY: drop into bash (readline history, arrow keys, `source`).
# No arguments, no TTY: run the image's PLI_DEFAULT_CMD (the CI path).
# Any arguments: run them verbatim, so `docker run ... <cmd>` still works.
set -euo pipefail

if [ "$#" -eq 0 ]; then
  # Interactive container: hand the terminal over to bash.
  if [ -t 0 ] && [ -t 1 ]; then
    exec bash
  fi
  # Non-interactive default: whatever the image is built to run.
  exec bash -c "${PLI_DEFAULT_CMD:?PLI_DEFAULT_CMD is not set}"
fi

exec "$@"
