#!/usr/bin/env bash
# Local JVM integration runner: compiles the Java fixtures, launches the JVM,
# injects vmhook.dll, waits for the suite to finish, and prints the results.
# Usage: etc/run_jvm_test.sh [build-dir]   (default build-dir = build-werror)
set -uo pipefail

REPO="C:/repos/cpp/vmhook"
BUILD="${1:-build-werror}"
RUN="/tmp/vmtest_run"
OUT="/tmp/vmtest_out"

cd "$REPO" || exit 2

# Kill any stray JVM from a previous run.
powershell -c "Get-Process java -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1

# Compile fixtures.
rm -rf "$OUT" && mkdir -p "$OUT"
if ! javac -d "$OUT" example/vmhook/*.java 2>/tmp/javac_err.txt; then
    echo "[HARNESS] javac FAILED:"; cat /tmp/javac_err.txt; exit 3
fi

# Stage DLL + injector.
rm -rf "$RUN" && mkdir -p "$RUN"
cp "$REPO/$BUILD/bin/vmhook.dll" "$REPO/$BUILD/bin/injector.exe" "$RUN/" || {
    echo "[HARNESS] missing DLL/injector in $BUILD/bin"; exit 4; }

cd "$RUN" || exit 2

# Launch the JVM (headless) and inject.
nohup java -cp "$OUT" vmhook.Main > java_out.txt 2> java_err.txt &
sleep 5
./injector.exe > inject.txt 2>&1
INJ=$?
echo "[HARNESS] injector exit=$INJ"
cat inject.txt

# Wait up to 60s for the JVM to exit (the DLL sets stopJVM at the end).
for i in $(seq 1 60); do
    if ! powershell -c "Get-Process java -ErrorAction SilentlyContinue" >/dev/null 2>&1; then break; fi
    if powershell -c "Get-Process java -ErrorAction SilentlyContinue" 2>/dev/null | grep -q java; then sleep 1; else break; fi
done
powershell -c "Get-Process java -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1

echo "[HARNESS] === java stdout ==="
cat java_out.txt 2>/dev/null
echo "[HARNESS] === java stderr (head) ==="
head -20 java_err.txt 2>/dev/null
echo "[HARNESS] === test_results.txt ==="
if [ -f test_results.txt ]; then
    PASS=$(grep -c '^\[PASS\]' test_results.txt)
    FAIL=$(grep -c '^\[FAIL\]' test_results.txt)
    echo "[HARNESS] PASS=$PASS FAIL=$FAIL"
    grep '^\[FAIL\]' test_results.txt | head -40
    [ "$FAIL" -eq 0 ] && [ "$PASS" -gt 0 ] && echo "[HARNESS] RESULT: ALL PASS"
else
    echo "[HARNESS] test_results.txt MISSING"
fi
