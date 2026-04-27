@echo off
setlocal

::
:: run_tests.bat — compile, launch with -Xint, inject VMHook.dll, read results.
::
:: -Xint disables JIT compilation so every method call goes through the interpreter.
:: This guarantees that VMHook hooks always fire, regardless of injection timing.
::
:: For production usage (without -Xint) inject BEFORE the target method is called
:: ~200 times (C1 compilation threshold); after that the JIT takes over and
:: interpreter hooks are bypassed.
::

if not exist "out" mkdir out

echo [*] Compiling Java sources...
javac -d out ^
    src\vmhook\example\Player.java ^
    src\vmhook\example\TestTarget.java ^
    src\vmhook\example\Main.java
if errorlevel 1 ( echo [ERROR] Compilation failed. & pause & exit /b 1 )
echo [OK] Compiled.
echo.

echo [*] Launching example target with -Xint (interpreter only)...
start "VMHook Test Target" ^
    "C:\Program Files\Eclipse Adoptium\jdk-21.0.10.7-hotspot\bin\java.exe" ^
    -Xint ^
    -cp out ^
    vmhook.example.Main

echo [*] Waiting 4 seconds for JVM to initialise...
timeout /t 4 /nobreak >nul

echo [*] Injecting VMHook.dll...
start "VMHook Injector" ..\build\Injector.exe

echo [*] Waiting 10 seconds for tests to complete...
timeout /t 10 /nobreak >nul

echo [*] Test output (from log.txt):
if exist "..\log.txt" (
    findstr /C:"[PASS]" /C:"[FAIL]" /C:"RESULTS" /C:"ALL TESTS" /C:"SOME TESTS" "..\log.txt"
) else (
    echo [ERROR] log.txt not found. Did the injection succeed?
)

echo.
echo [*] Press any key to exit (this will leave the Java process running).
pause >nul
