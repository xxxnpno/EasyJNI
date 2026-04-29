@echo off
setlocal

::
:: run_tests.bat — compile, launch (with JIT enabled), inject VMHook.dll, read results.
::
:: VMHook now deoptimises JIT-compiled methods at hook-install time:
::   - _code is cleared (dispatch reverts to interpreter)
::   - _from_interpreted_entry is reset to the patched i2i stub
::   - _from_compiled_entry is reset to the c2i adapter
:: Hooks therefore fire even if the target method was already compiled at injection time.
::
:: Remaining edge case: compiled callers with stale monomorphic inline caches (ICs) still
:: call the old nmethod directly until HotSpot repairs the IC at the next safe-point.
:: In the test loop (100 ticks/s + Thread.sleep) this typically resolves within 1-2 ticks.
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

echo [*] Launching example target (JIT enabled)...
start "VMHook Test Target" ^
    "C:\Program Files\Eclipse Adoptium\jdk-21.0.10.7-hotspot\bin\java.exe" ^
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
