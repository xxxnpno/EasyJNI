@echo off
setlocal

:: Compile
if not exist "out" mkdir out
javac -d out src\vmhook\example\Player.java src\vmhook\example\Main.java
if errorlevel 1 ( echo [ERROR] Compilation failed. & pause & exit /b 1 )

echo [OK] Compiled to out\
echo.

:: Run — keep the window title readable so Injector.exe can confirm the right PID
title VMHook Example Target
java -cp out vmhook.example.Main

pause
