# ZZPlug - the USB logic on the host: native gcc in WSL, test/simtest.c + test/sim.c + src/core.c
$ErrorActionPreference = "Stop"
$wslRoot = ($PSScriptRoot -replace '^([A-Za-z]):', { '/mnt/' + $_.Groups[1].Value.ToLower() }) -replace '\\','/'
wsl -e bash -lc "cd '$wslRoot' && mkdir -p build && gcc -O2 -Wall -Wextra -Wno-unused-parameter -o build/simtest test/simtest.c test/sim.c src/core.c && ./build/simtest"
