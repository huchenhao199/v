@echo off
rem build_release.bat -- 编一个能直接发给别人的 fp.exe，再打成一个 zip。
rem 和 build.bat 的区别：用 /MT 静态链接 CRT，别人不用装 VC++ 运行库也能跑。
rem Keep this file pure ASCII except the echo lines: cmd.exe needs them.
setlocal
cd /d "%~dp0"
set "OUT=fp.exe"
set "LOG=build\build.log"
set "ZIP=fp-meadow-v1.0.0-win64.zip"
set "VCVARS="

if not exist build mkdir build
if exist "%OUT%" del /q "%OUT%"
if exist "%LOG%" del /q "%LOG%"
if exist "%ZIP%" del /q "%ZIP%"

for %%E in (Community Professional Enterprise BuildTools) do call :tryvs "%%E"
if not defined VCVARS goto NOCOMPILER

call "%VCVARS%" >nul
cl /nologo /utf-8 /O2 /MT /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /Fe:%OUT% /Fo:build\ ^
   src\main.cpp user32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib > "%LOG%" 2>&1

findstr /i /c:"error" "%LOG%" >nul && goto FAIL
if not exist "%OUT%" goto FAIL
findstr /i /c:"warning" "%LOG%" >nul && echo [!] WARNING -- see %LOG%
echo [*] 编译好了: %OUT%

rem 打包：exe + README + LICENSE
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%OUT%','README.md','LICENSE' -DestinationPath '%ZIP%' -Force"
if not exist "%ZIP%" goto ZIPFAIL

echo.
echo   ==========================================================
echo    RELEASE OK
echo      %cd%\%OUT%
echo      %cd%\%ZIP%
echo   ==========================================================
echo.
echo   下一步：把这个 zip 拖到 GitHub 的 Releases 页面上就行了。
echo.
pause
exit /b 0

:FAIL
echo.
echo [x] 编译失败。
if exist "%LOG%" type "%LOG%"
echo.
pause
exit /b 1

:ZIPFAIL
echo [x] 编译好了，但打包失败（看看 PowerShell 那句）。
pause
exit /b 1

:NOCOMPILER
echo [x] 没找到 MSVC（需要"使用 C++ 的桌面开发"）。
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
