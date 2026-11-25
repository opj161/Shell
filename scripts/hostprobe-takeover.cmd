@echo off
rem Runs hostprobe in takeover mode against the x64 build in src\bin\x64, with
rem the per-user COM overrides it needs and their removal afterwards.
rem
rem   scripts\hostprobe-takeover.cmd <log|con> [filter] [runs]
rem
rem Must run UNELEVATED. "A process elevated by using the UAC mechanism does not
rem load per-user classes during COM activations"
rem (https://learn.microsoft.com/windows/win32/com/the-com-elevation-moniker), so
rem from an elevated shell the shell-namespace scenarios load the installed
rem shell.dll and test that build instead. An agent shell is elevated here:
rem start this through Explorer, which runs it at medium integrity, and write the
rem log to a file:  explorer.exe "<a .cmd that calls this with a log path>".
rem
rem All three of Shell's classes are redirected, and the Windows 11 TreatAs
rem redirect is shadowed per user: with only the context-menu class overridden,
rem COM still activated the installed copy through {86ca1aa0}'s machine-wide
rem TreatAs (measured 2026-11-25). hostprobe fails a takeover run that ends with
rem more than one shell.dll mapped. docs/refactor/13-implementation-handoff.md 4.
setlocal EnableDelayedExpansion
set LOG=%~1
set FILTER=%~2
set RUNS=%~3
if "%LOG%"=="" set LOG=con
if "%RUNS%"=="" set RUNS=1
cd /d "%~dp0.."

set DLL=%CD%\src\bin\x64\shell.dll
set CLASSES=HKCU\SOFTWARE\Classes\CLSID
set REDIRECT={86ca1aa0-34aa-4e8b-a509-50c905bae2a2}

copy /y src\bin\shell.nss src\bin\x64\shell.nss >nul
xcopy /e /i /y /q src\bin\imports src\bin\x64\imports >nul
for %%C in ({BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1} {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF2} {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3}) do (
  reg add "%CLASSES%\%%C\InprocServer32" /ve /t REG_SZ /d "%DLL%" /f >nul
  reg add "%CLASSES%\%%C\InprocServer32" /v ThreadingModel /t REG_SZ /d Apartment /f >nul
)
reg add "%CLASSES%\%REDIRECT%\TreatAs" /ve /t REG_SZ /d "{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}" /f >nul

for /l %%i in (1,1,%RUNS%) do (
  echo === run %%i>> "%LOG%"
  if "%FILTER%"=="" (
    src\bin\x64\hostprobe.exe --takeover --verify src\tests\hostprobe\fixtures >> "%LOG%" 2>&1
  ) else (
    src\bin\x64\hostprobe.exe --takeover %FILTER% >> "%LOG%" 2>&1
  )
  echo === exit !errorlevel! run %%i>> "%LOG%"
)

rem Mandatory: leaving these points every shell activation for this user at a dev build.
for %%C in ({BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1} {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF2} {BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF3} %REDIRECT%) do (
  reg delete "%CLASSES%\%%C" /f >nul 2>&1
)
del /q src\bin\x64\shell.nss
rmdir /s /q src\bin\x64\imports
echo === done>> "%LOG%"
