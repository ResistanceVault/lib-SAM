@echo off
setlocal

set "ROOT=%~dp0"
set "TTS=%ROOT%tts.exe"

if not exist "%TTS%" (
    echo Missing executable: "%TTS%"
    exit /b 1
)

echo Generating English demo...
"%TTS%" "Empty your mind, be formless, shapeless, like water. If you put water into a cup, it becomes the cup. You put water into a bottle and it becomes the bottle. You put it in a teapot it becomes the teapot. Now, water can flow or it can crash. Be water, my friend." -o "%ROOT%demo-en.aiff" --lang en
if errorlevel 1 exit /b 1

echo Generating Phone English demo...
"%TTS%" "Empty your mind, be formless, shapeless, like water. If you put water into a cup, it becomes the cup. You put water into a bottle and it becomes the bottle. You put it in a teapot it becomes the teapot. Now, water can flow or it can crash. Be water, my friend." -o "%ROOT%demo-phone-en.aiff" --lang en --phone
if errorlevel 1 exit /b 1

echo Done.
echo Created:
echo   "%ROOT%demo-en.aiff"
echo   "%ROOT%demo-fr.aiff"

endlocal
