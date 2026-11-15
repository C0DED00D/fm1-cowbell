@echo off
rem Double-click to play COWBELL in the FM-1 emulator. "play.cmd fresh" starts from the last build again.
cd /d "%~dp0"
wsl -d Ubuntu -- bash tools/play.sh %*
if errorlevel 1 (pause & exit /b 1)
echo.
echo  COWBELL on the emulated FM-1 (close the window or press Esc to quit)
echo  Mouse: click keys and buttons; drag or scroll a knob to turn it. MASTER is the volume.
echo  Keyboard: A W S E D F T G Y H U J K O L P ; '  = play the cowbell from C4 up (hold: ratchet at MORE)
echo            Space PLAY/STOP the pattern   0 SEQ: step mode on/off (the keys become the 16 steps)
echo            Z / X octave (in step mode: part)   R REC   6 GLO(FUNC)   8 SAVE   7 HOME
echo            Left/Right FEVER   Up/Down PRESCRIPTION   , . MORE   (small knobs: mouse)
echo  A MIDI keyboard plugged into the PC plays it too.
echo.
emu\fm1-live.exe --skin cowbell --fwsc emu\fm1-dock.fwsc --flash out\play_flash.bin
if errorlevel 1 pause
