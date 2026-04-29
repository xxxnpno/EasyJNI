param(
    [int]$JavaMajor   = 21,
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$outDir = Join-Path $PSScriptRoot "out"
$logFile = Join-Path $RepoRoot "log.txt"
$injector = Join-Path $RepoRoot "build\Injector.exe"
$javaSources = @(
    (Join-Path $PSScriptRoot "src\Player.java"),
    (Join-Path $PSScriptRoot "src\TestTarget.java"),
    (Join-Path $PSScriptRoot "src\Main.java")
)

function Stop-Java {
    Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in @("java", "javaw") } |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

Write-Host "[CI] Cleaning old state..."
Stop-Java
if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
if (Test-Path $logFile) { Remove-Item $logFile -Force }
New-Item -ItemType Directory -Path $outDir | Out-Null

Write-Host "[CI] Compiling Java sources (JDK $JavaMajor)..."
if ($JavaMajor -le 8) {
    & javac -source 8 -target 8 -d $outDir @javaSources
} else {
    & javac --release $JavaMajor -d $outDir @javaSources
}
if ($LASTEXITCODE -ne 0) {
    throw "javac failed"
}

if (-not (Test-Path $injector)) {
    throw "Injector not found: $injector"
}

$javaProc = $null
try {
    Write-Host "[CI] Starting JVM target..."
    $javaProc = Start-Process -FilePath "java" -ArgumentList @("-cp", $outDir, "vmhook.example.Main") -PassThru
    Write-Host "[CI] JVM PID: $($javaProc.Id)"

    Write-Host "[CI] Waiting 10 s for JVM to initialise..."
    Start-Sleep -Seconds 10

    if ($javaProc.HasExited) {
        throw "JVM process exited before injection (exit code $($javaProc.ExitCode))"
    }
    # Retry injection up to 3 times in case Defender delays the first attempt
    $injected = $false
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        Write-Host "[CI] Injection attempt $attempt..."
        if (Test-Path $logFile) { Remove-Item $logFile -Force }

        & $injector | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "Injector exited with code $LASTEXITCODE"
        }

        Write-Host "[CI] Waiting up to 60 s for test results..."
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline) {
            if (Test-Path $logFile) {
                $content = Get-Content $logFile -ErrorAction SilentlyContinue
                if ($content -match "RESULTS:") { $injected = $true; break }
            }
            Start-Sleep -Milliseconds 500
        }

        if ($injected) { break }
        Write-Host "[CI] Attempt $attempt: no results yet — retrying..."
        Start-Sleep -Seconds 3
    }

    if (-not (Test-Path $logFile)) {
        throw "log.txt not created after $attempt attempts — injection was blocked"
    }

    $lines = Get-Content $logFile
    $summary = $lines | Select-String "RESULTS:" | Select-Object -Last 1
    if (-not $summary) {
        throw "No RESULTS line found in log.txt"
    }

    $failCount = @($lines | Select-String "\[FAIL\]").Count
    $passCount = @($lines | Select-String "\[PASS\]").Count

    Write-Host "[CI] --- full log.txt ---"
    $lines | ForEach-Object { Write-Host $_ }
    Write-Host "[CI] --- end log.txt ---"
    Write-Host "[CI] Summary: $($summary.Line)"
    Write-Host "[CI] PASS lines: $passCount"
    Write-Host "[CI] FAIL lines: $failCount"

    if ($failCount -gt 0) {
        throw "Tests reported failures"
    }
}
finally {
    Stop-Java
}

Write-Host "[CI] Tests passed."

