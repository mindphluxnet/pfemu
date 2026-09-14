@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /O2 /W3 /wd4996 /Fe:pfemu.exe src/cpu.c src/vga.c src/dev.c src/bios.c src/dos.c src/sound.c src/main.c user32.lib gdi32.lib winmm.lib
del *.obj