@echo off
rem Build a release into its own output folder (never the default one, which a running service may
rem lock), then stage and zip it with package.ps1: dist\DeckBtService-<version>.zip.
setlocal
set "ROOT=%~dp0.."
for %%f in (install.cmd uninstall.cmd collect-diagnostics.cmd install.ps1 uninstall.ps1 collect-diagnostics.ps1 README-install.md) do (
  if not exist "%ROOT%\packaging\%%f" (
    echo ERROR: missing release input "%ROOT%\packaging\%%f"
    exit /b 1
  )
)
if not exist "%ROOT%\LICENSE" (
  echo ERROR: missing release input "%ROOT%\LICENSE"
  exit /b 1
)
set "DECKBT_BUILD_OUT=%~dp0_build\package-release"
call "%~dp0build.cmd"
if errorlevel 1 exit /b 1
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1" -Root "%ROOT%" -BuildOut "%DECKBT_BUILD_OUT%"
exit /b %errorlevel%
