#!/bin/bash
# Run the Windows PowerShell build under Wine.
# Wine only gives pwsh a usable console through a pty; with plain pipes pwsh
# silently drops stdout and exit codes, so run it under script(1). The args are
# re-quoted for the bash that script starts, because script's own sh cannot
# parse bash's $'...' quoting for multi-line -Command strings.
#
# Interactive sessions drop PSReadLine: under Wine its cursor handling
# scrambles pasted multi-line text (lines arrive reversed) and Tab completion.
# The plain console host keeps history, line editing and completion working.

# -Command/-File keep their normal semantics; only a bare interactive shell is
# switched to the plain console host.
interactive=1
for arg in "$@"; do
  case "${arg,,}" in
    -command|-c|-commandwithargs|-encodedcommand|-e|-file|-f|-configurationfile) interactive=0 ;;
  esac
done

# A real terminal on both ends is required; piped stdin must stay untouched.
if [ "$interactive" -eq 1 ] && [ -t 0 ] && [ -t 1 ]; then
  set -- "$@" -NoExit -Command 'Remove-Module PSReadLine'
fi

cmd=$(printf '%q ' wine64 /opt/powershell/pwsh.exe "$@")
PWSH_CMD=$cmd exec script -qec 'bash -c "$PWSH_CMD"' /dev/null
