@echo off
rem convert_flashback.bat - turns Flashback .raw clips into MP4 videos with ffmpeg.
rem
rem Usage:
rem   - Drag one or more clip_*.raw files onto this file, or
rem   - Double-click it to convert every clip_*.raw in the same folder as this file.
rem
rem Each clip's resolution and frame rate are read from the .txt saved next to it.
rem Clips that already have an .mp4 are skipped.

setlocal

where ffmpeg >nul 2>nul
if errorlevel 1 (
    echo ffmpeg was not found. Install it from https://ffmpeg.org and add its bin folder to PATH.
    pause
    exit /b 1
)

set /a converted=0, skipped=0, failed=0

if "%~1"=="" (
    for %%F in ("%~dp0clip_*.raw") do call :convert "%%~fF"
) else (
    for %%F in (%*) do call :convert "%%~fF"
)

echo.
echo Done: %converted% converted, %skipped% skipped, %failed% failed.
pause
exit /b 0


:convert
set "raw=%~1"
set "base=%~dpn1"
set "name=%~nx1"
set "res="
set "fps="

if /i not "%~x1"==".raw" (
    echo Skipping %name%: not a .raw file.
    set /a skipped+=1
    exit /b 0
)

if exist "%base%.mp4" (
    echo Skipping %name%: %~n1.mp4 already exists.
    set /a skipped+=1
    exit /b 0
)

if not exist "%base%.txt" (
    echo Skipping %name%: %~n1.txt is missing, so its resolution and frame rate are unknown.
    set /a failed+=1
    exit /b 0
)

rem "Resolution:   400x240" and "Frame rate:   30 fps"
for /f "tokens=2" %%a in ('findstr /b /c:"Resolution:" "%base%.txt"') do set "res=%%a"
for /f "tokens=3" %%a in ('findstr /b /c:"Frame rate:" "%base%.txt"') do set "fps=%%a"

if not defined res goto :badtxt
if not defined fps goto :badtxt

echo Converting %name% (%res%, %fps% fps)...
ffmpeg -hide_banner -loglevel error -n -f rawvideo -pixel_format rgb565le -video_size %res% -framerate %fps% -i "%raw%" -vf "scale=iw*2:ih*2:flags=neighbor" -c:v libx264 -crf 12 -pix_fmt yuv420p "%base%.mp4"
if errorlevel 1 (
    echo   Failed.
    set /a failed+=1
) else (
    echo   Saved %~n1.mp4
    set /a converted+=1
)
exit /b 0

:badtxt
echo Skipping %name%: couldn't read the resolution or frame rate from %~n1.txt.
set /a failed+=1
exit /b 0
