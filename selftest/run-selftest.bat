@echo off
rem Coyote verification: runs the conformance suite (PVM, GC stress, native), the self-test apps and the project flow,
rem then writes selftest-report.txt next to this folder. Send that file back.
rem   run-selftest.bat                 uses the Coyote found on the PATH
rem   run-selftest.bat --bin "C:\Program Files\Coyote\bin"
rem   run-selftest.bat --quick         skips the slow GC-stress pass
rem   run-selftest.bat --ui            also runs the interactive part (dialogs: you answer questions)
setlocal
cd /d "%~dp0.."
where coyote >nul 2>nul
if errorlevel 1 (
  if not exist "%ProgramFiles%\Coyote\bin\coyote.exe" (
    echo coyote.exe was not found on the PATH. Install Coyote first, or open a terminal where it works.
    exit /b 2
  )
  set "PATH=%ProgramFiles%\Coyote\bin;%PATH%"
)
echo Running the Coyote verification. This takes a few minutes; do not close the window.
coyote run selftest\Runner\src\main.coy -- %*
echo.
echo Done. Send selftest-report.txt (in %CD%) back. If something failed, also zip the folder selftest-work.
pause
