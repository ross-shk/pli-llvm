#!/bin/sh
# Shared entrypoint for the pli test images.
# No arguments + a TTY: drop into sh.
# No arguments, no TTY: run the image's PLI_DEFAULT_CMD (the CI path).
# Any arguments: run them verbatim, so `docker run ... <cmd>` still works.
set -eu

if [ "$#" -eq 0 ]; then
  if [ -t 0 ] && [ -t 1 ]; then
    exec sh
  fi
  exec sh -c "${PLI_DEFAULT_CMD:?PLI_DEFAULT_CMD is not set}"
fi

exec "$@"
