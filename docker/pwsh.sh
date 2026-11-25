#!/bin/bash
# Run the Windows PowerShell build under Wine.
# Wine only gives pwsh a usable console through a pty; with plain pipes pwsh
# silently drops stdout and exit codes, so run it under script(1). The args are
# re-quoted for the bash that script starts, because script's own sh cannot
# parse bash's $'...' quoting for multi-line -Command strings.
cmd=$(printf '%q ' wine64 /opt/powershell/pwsh.exe "$@")
PWSH_CMD=$cmd exec script -qec 'bash -c "$PWSH_CMD"' /dev/null
