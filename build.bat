@echo off
rem build.bat -- build fp.exe (first-person view, Direct3D 11)
rem Keep this file pure ASCII with CRLF endings: cmd.exe needs them.
setlocal
cd /d "%~dp0"
set "OUT=fp.exe"
set "LOG=build\build.log"
set "VCVARS="
if not exist build mkdir build
if exist "%OUT%" del /q "%OUT%"
if exist "%LOG%" del /q "%LOG%"

for %%E in (Community Professional Enterprise BuildTools) do call :tryvs "%%E"
if not defined VCVARS goto NOCOMPILER

echo [*] Compiler : MSVC
call "%VCVARS%" >nul
cl /nologo /utf-8 /O2 /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /Fe:%OUT% /Fo:build\ ^
   src\main.cpp user32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib > "%LOG%" 2>&1

findstr /i /c:"error" "%LOG%" >nul && goto FAIL
if not exist "%OUT%" goto FAIL
findstr /i /c:"warning" "%LOG%" >nul && echo [!] WARNING found -- see %LOG%
echo.
echo   ==========================================================
echo    BUILD OK
echo      %cd%\%OUT%
echo   ==========================================================
echo   (compiler output: %LOG%)
echo.
pause
exit /b 0

:FAIL
echo.
echo [x] Build FAILED.
if exist "%LOG%" type "%LOG%"
echo.
pause
exit /b 1

:NOCOMPILER
echo [x] No MSVC found (need "Desktop development with C++").
pause
exit /b 1

:tryvs
if defined VCVARS exit /b 0
for %%V in (18 2022 2019) do call :onevs "%%V" "%~1"
exit /b 0

:onevs
if defined VCVARS exit /b 0
set "P1=%ProgramFiles%\Microsoft Visual Studio\%~1\%~2%\VC\Auxiliary\Build\vcvars64.bat"
if exist "%P1%" set "VCVARS=%P1%"
if defined VCVARS exit /b 0
set "P2=%ProgramFiles(x86)%\Microsoft Visual Studio\%~1\%~2%\VC\Auxiliary\Build\vcvars64.bat"
if exist "%P2%" set "VCVARS=%P2%"
exit /b 0
