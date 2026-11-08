#!/bin/sh
set -e
PLI=${PLI:-./build/cmake/pli}
$PLI tests/core/display_event.pli -o /tmp/display_event 2>&1
/tmp/display_event