<#
.SYNOPSIS
    Run the VMHook unit test suite against every configured JDK.

.DESCRIPTION
    For each JDK in the $jdks map below:
      1. Compile the Java sources with that JDK's javac.
      2. Launch java with JIT enabled (no -Xint).
      3. Wait 5 s for the JVM to load classes and run the hook target long enough
         for JIT compilation to kick in (~200 calls at 100/s = ~2 s).
      4. Run Injector.exe, which exits immediately after injection.
         VMHook deoptimises any already-compiled method at hook-install time:
           - _code is cleared (dispatch reverts to interpreter)
           - _from_interpreted_entry → i2i stub (our patch)
           - _from_compiled_entry   → c2i adapter
      5. Wait 10 s for the DLL test suite to complete.
      6. Parse log.txt for PASS / FAIL / SKIP / RESULTS lines.
      7. Print a colour-coded summary and exit 1 if any version fails.

.NOTES
    Run from the repo root:  powershell -ExecutionPolicy Bypass -File example\test_all_jdks.ps1

    To add a new JDK:
      1. Download the Adoptium Temurin ZIP from https://adoptium.net/temurin/releases/
         (or any OpenJDK distribution — must be HotSpot, not OpenJ9/GraalVM).
      2. Extract to C:\jdks\jdk-<major>\ (so bin\java.exe exists).
      3. Add an entry to the $jdks hashtable below.
      4. Re-run this script.

    Known limitations:
      - Compiled callers with stale monomorphic inline caches (ICs) may bypass the hook
        for 1-2 ticks until HotSpot repairs the IC at the next safe-point. In the test
        loop (Thread.sleep + 100/s) this resolves automatically within the 10 s window.
      - JDK 9 and 10 are not included: Adoptium Temurin does not publish binaries
        for those EOL versions.  They are structurally identical to JDK 11 for the
        purposes of gHotSpotVMStructs, so JDK 11 coverage is sufficient.
#>

param(
    [string]$InjectorExe  = "$PSScriptRoot\..\build\Injector.exe",
    [string]$LogFile      = "$PSScriptRoot\..\log.txt",
    [string]$ClasspathOut = "$PSScriptRoot\out"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ─── JDK registry ────────────────────────────────────────────────────────────
# Add or remove entries here as new JDKs are installed.
# Key   = major version number (used for display only)
# Value = path to the JDK root directory (must contain bin\java.exe and bin\javac.exe)
$jdks = [ordered]@{
    8  = "C:\Program Files\Eclipse Adoptium\jdk-8.0.482.8-hotspot"
    11 = "C:\jdks\jdk-11"
    17 = "C:\jdks\jdk-17"
    21 = "C:\Program Files\Eclipse Adoptium\jdk-21.0.10.7-hotspot"
    25 = "C:\jdks\jdk-25"
    26 = "C:\jdks\jdk-26"
}

# ─── Source files ─────────────────────────────────────────────────────────────
$java_sources = @(
    "$PSScriptRoot\src\Player.java"
    "$PSScriptRoot\src\TestTarget.java"
    "$PSScriptRoot\src\Main.java"
)

# ─── Helpers ─────────────────────────────────────────────────────────────────
function Write-Color([string]$text, [ConsoleColor]$color) {
    $prev = $Host.UI.RawUI.ForegroundColor
    $Host.UI.RawUI.ForegroundColor = $color
    Write-Host $text
    $Host.UI.RawUI.ForegroundColor = $prev
}

function Kill-Java {
    Get-Process | Where-Object { $_.Name -in "java","javaw" } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

# ─── Pre-flight ───────────────────────────────────────────────────────────────
if (-not (Test-Path $InjectorExe)) {
    Write-Color "[ERROR] Injector.exe not found at: $InjectorExe" Red
    Write-Color "        Build Release|x64 first." Red
    exit 1
}

$overall_pass = $true
$results_table = @()

# ─── Main loop ────────────────────────────────────────────────────────────────
foreach ($entry in $jdks.GetEnumerator()) {
    $major    = $entry.Key
    $jdk_root = $entry.Value
    $java_exe  = "$jdk_root\bin\java.exe"
    $javac_exe = "$jdk_root\bin\javac.exe"

    Write-Host ""
    Write-Color "════════════════════════════════════════════════════════" Cyan
    Write-Color "  Testing JDK $major  ($jdk_root)" Cyan
    Write-Color "════════════════════════════════════════════════════════" Cyan

    # Validate JDK exists
    if (-not (Test-Path $java_exe)) {
        Write-Color "  [SKIP] java.exe not found — JDK $major not installed." Yellow
        Write-Color "         Download from https://adoptium.net/temurin/releases/" Yellow
        Write-Color "         and extract to: $jdk_root" Yellow
        $results_table += [pscustomobject]@{ JDK="$major"; Passed="-"; Failed="-"; Status="NOT_INSTALLED" }
        continue
    }

    # 1. Compile
    Write-Host "  [1/5] Compiling with JDK $major javac..."
    if (Test-Path $ClasspathOut) { Remove-Item $ClasspathOut -Recurse -Force }
    New-Item -ItemType Directory -Force $ClasspathOut | Out-Null

    # --release was added in JDK 9; use -source/-target for JDK 8
    if ($major -le 8) {
        $javac_args = @("-source", "8", "-target", "8", "-d", $ClasspathOut) + $java_sources
    } else {
        $javac_args = @("--release", "$major", "-d", $ClasspathOut) + $java_sources
    }
    $compile = & $javac_exe @javac_args 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Color "  [FAIL] Compilation failed:" Red
        $compile | ForEach-Object { Write-Host "    $_" }
        $results_table += [pscustomobject]@{ JDK="$major"; Passed=0; Failed=1; Status="COMPILE_FAIL" }
        $overall_pass = $false
        continue
    }
    Write-Host "         OK"

    # 2. Kill any stale java processes
    Kill-Java

    # 3. Launch the target process (JIT enabled — no -Xint)
    Write-Host "  [2/5] Launching target (JIT enabled)..."
    $java_proc = Start-Process -FilePath $java_exe `
        -ArgumentList @("-cp", $ClasspathOut, "vmhook.example.Main") `
        -PassThru -WindowStyle Minimized

    # Wait up to 10 s for the JVM to appear
    $deadline = (Get-Date).AddSeconds(10)
    $confirmed = $false
    while ((Get-Date) -lt $deadline) {
        if (-not $java_proc.HasExited) { $confirmed = $true; break }
        Start-Sleep -Seconds 1
    }
    if (-not $confirmed) {
        Write-Color "  [FAIL] JVM process exited immediately." Red
        $results_table += [pscustomobject]@{ JDK="$major"; Passed=0; Failed=1; Status="JVM_CRASH" }
        $overall_pass = $false
        continue
    }
    Write-Host "         PID=$($java_proc.Id)"

    # 4. Give classes time to load, then inject
    Write-Host "  [3/5] Waiting 5 s for class loading..."
    Start-Sleep -Seconds 5

    # Clear old log
    if (Test-Path $LogFile) { Remove-Item $LogFile -Force }

    Write-Host "  [4/5] Injecting VMHook.dll..."
    & $InjectorExe | Out-Null   # exits immediately now

    # 5. Wait for tests to complete
    Write-Host "  [5/5] Waiting 10 s for test suite..."
    Start-Sleep -Seconds 10

    # 6. Parse log.txt
    if (-not (Test-Path $LogFile)) {
        Write-Color "  [FAIL] log.txt was not created — injection failed." Red
        $results_table += [pscustomobject]@{ JDK="$major"; Passed=0; Failed=1; Status="INJECT_FAIL" }
        $overall_pass = $false
        Kill-Java
        continue
    }

    $log_lines = Get-Content $LogFile
    $pass_count = @($log_lines | Select-String "\[PASS\]").Count
    $fail_count = @($log_lines | Select-String "\[FAIL\]").Count
    $results_line = $log_lines | Select-String "RESULTS:" | Select-Object -Last 1

    # Print individual lines
    $log_lines | Where-Object { $_ -match "PASS|FAIL|SKIP|RESULT|ALL TESTS|SOME TESTS" } |
        ForEach-Object {
            if ($_ -match "FAIL")  { Write-Color "  $_" Red    }
            elseif ($_ -match "PASS") { Write-Color "  $_" Green  }
            else                   { Write-Host   "  $_" }
        }

    $status = if ($fail_count -eq 0 -and $pass_count -gt 0) { "PASS" } else { "FAIL" }
    if ($status -eq "FAIL") { $overall_pass = $false }

    $results_table += [pscustomobject]@{
        JDK    = "$major"
        Passed = $pass_count
        Failed = $fail_count
        Status = $status
    }

    Kill-Java
}

# ─── Summary ──────────────────────────────────────────────────────────────────
Write-Host ""
Write-Color "════════════════════════════════════════════════════════" Cyan
Write-Color "  MULTI-JDK TEST SUMMARY" Cyan
Write-Color "════════════════════════════════════════════════════════" Cyan
$results_table | Format-Table -AutoSize | Out-String | Write-Host

if ($overall_pass) {
    Write-Color "  ALL JDK VERSIONS PASSED" Green
    exit 0
} else {
    Write-Color "  SOME JDK VERSIONS FAILED" Red
    exit 1
}
