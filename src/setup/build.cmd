@echo off
rem Thin wrapper over the canonical build. There is deliberately no second way
rem to produce a release package here.
rem
rem This script used to run `wix.exe build` directly, which skips Windows
rem Installer validation - MSBuild runs the stock MSI SDK ICEs for you, the
rem command line does not:
rem
rem   https://docs.firegiant.com/wix/tools/validation/
rem
rem It also deleted the .wixpdb straight after building, swallowed the exit code
rem so a failed build looked like a successful one, and ended in `pause`, which
rem hangs any non-interactive caller.
rem
rem build.ps1 builds src\Shell.sln, which contains setup\wix\setup.wixproj, so
rem the package produced here is the validated one.

setlocal

set "arch=%~1"
if "%arch%" == "" set "arch=x64"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\..\build.ps1" -Platform %arch%
exit /b %ERRORLEVEL%
