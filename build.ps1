# Build ZZPlug via amiga-gcc (Bebbo) in WSL -> out/ZZPlug (68020+, AmigaOS 3.1+)
$ErrorActionPreference = "Stop"
$flags = "-O2 -m68020 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer -s"
$wslRoot = ($PSScriptRoot -replace '^([A-Za-z]):', { '/mnt/' + $_.Groups[1].Value.ToLower() }) -replace '\\','/'
$cmd = "export PATH=/opt/amiga/bin:`$PATH; cd '$wslRoot' && mkdir -p out && m68k-amigaos-gcc $flags -o out/ZZPlug src/zzplug.c src/mailbox.c src/core.c"
wsl -e bash -lc $cmd
if ($LASTEXITCODE -eq 0) { Write-Host "Build OK -> out/ZZPlug" }
