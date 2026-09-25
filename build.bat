@echo off
rem If cl.exe is already on PATH (e.g. CI set up the compiler environment),
rem there is nothing to do. Otherwise locate vcvars64.bat: first the default
rem VS2019 Build Tools location, then whatever vswhere finds.
where cl >nul 2>&1
if %errorlevel% neq 0 (
  if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
  ) else (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
      if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    )
  )
)
where cl >nul 2>&1
if %errorlevel% neq 0 (
  echo No MSVC compiler found. Install Visual Studio 2019 Build Tools with the C++ toolchain.
  exit /b 1
)
rem The build identity the -verify object carries (src\verify.c), the same
rem one the Makefile writes. Without git history it says unknown.
set PFEMU_BUILD=unknown
for /f "usebackq delims=" %%i in (`git describe --always --dirty --abbrev^=12 2^>nul`) do set PFEMU_BUILD=%%i
>src\build.h echo #define PFEMU_BUILD "%PFEMU_BUILD%"
rc /nologo /fo pfemu.res res\pfemu.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /GL /W3 /wd4996 /Fe:pfemu.exe src/cpu.c src/vga.c src/dev.c src/bios.c src/dos.c src/sound.c src/main.c src/run.c src/launch.c src/launchcore.c src/online.c src/cfg.c src/fantasies.c src/release.c src/cdimage.c src/gog.c src/png.c src/replay.c src/snapshot.c src/verify.c src/vgafont.c src/video.c pfemu.res user32.lib gdi32.lib winmm.lib comctl32.lib comdlg32.lib shell32.lib winhttp.lib crypt32.lib advapi32.lib /link /LTCG /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
del *.obj
del pfemu.res