#!/usr/bin/env python3
import os
import signal
import subprocess
import sys


timeout = float(sys.argv[1])
outfile = sys.argv[2]
command = sys.argv[3:]

with open(outfile, "wb") as output:
    process = subprocess.Popen(
        command, stdout=output, stderr=subprocess.STDOUT, start_new_session=True
    )
    try:
        result = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        result = 124

sys.exit(result)
