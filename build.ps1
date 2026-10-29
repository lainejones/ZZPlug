# Build ZZPlug via amiga-gcc (Bebbo) in WSL -> out/ZZPlug (68020+, AmigaOS 3.1+)
$ErrorActionPreference = "Stop"
$flags = "-O2 -m68020 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer -s"
$wslRoot = ($PSScriptRoot -replace '^([A-Za-z]):', { '/mnt/' + $_.Groups[1].Value.ToLower() }) -replace '\\','/'
$cmd = "export PATH=/opt/amiga/bin:`$PATH; cd '$wslRoot' && mkdir -p out && m68k-amigaos-gcc $flags -o out/ZZPlug src/zzplug.c src/mailbox.c src/core.c src/hid.c src/report.c src/daemon.c src/msd.c src/device.c src/hidrep.c src/joy.c && m68k-amigaos-gcc $flags -o out/JoyTest src/joytest.c && m68k-amigaos-gcc $flags -o out/ZZPlugPrefs src/prefs.c && m68k-amigaos-gcc $flags -o out/KeyTest src/keytest.c"
wsl -e bash -lc $cmd
if ($LASTEXITCODE -eq 0) { Write-Host "Build OK -> out/ZZPlug" }
