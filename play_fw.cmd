@echo off
rem Play the COWBELL *firmware* (fw/, what goes on the FM-1) in the emulator, its samples already loaded.
rem "play_fw.cmd fresh" starts again from the last build. play.cmd plays the fm1-dock module instead.
cd /d "%~dp0"
if "%1"=="fresh" del out\fw_play_flash.bin 2>nul
if not exist out\fw_play_flash.bin python tools\emu_flash.py out\fw_play_flash.bin
if errorlevel 1 (pause & exit /b 1)
echo.
echo  COWBELL firmware on the emulated FM-1 (close the window or press Esc to quit)
echo  Keys play; 0 = SEQ (step mode); Space PLAY; arrows FEVER / PRESCRIPTION; , . MORE; mouse for the rest.
echo.
emu\fm1-live.exe --skin cowbell --fwsc fw\build\COWBELL.fwsc --flash out\fw_play_flash.bin
if errorlevel 1 pause
