# launch_minecraft.ps1
# Replicates MultiMC's LauncherPart method:
#   1. Launch java.exe with NewLaunch.jar (org.multimc.EntryPoint)
#   2. Feed the launch protocol via stdin (BOM-free ASCII temp file)
#
# Uses java.exe (not javaw.exe) so the process keeps running correctly
# when launched from a script context. A minimized console window will
# appear briefly and disappear once Minecraft takes over. Minimize it.
#
# Fixes applied during development:
#   - natives/ dir missing        -> extracted from the four native JARs
#   - path-with-spaces arg split  -> .bat handles quoting + 0< redirect
#   - UTF-8 BOM corrupts first key -> written with ASCII encoding (no BOM)
#   - duplicate --width/--height  -> removed from param list; windowParams handles them

Set-StrictMode -Off
$ErrorActionPreference = 'Stop'

$java    = 'C:\Program Files\Eclipse Adoptium\jdk-21.0.10.7-hotspot\bin\java.exe'
$mmc     = 'C:\Program Files\mmc-develop-win32\MultiMC'
$lib     = "$mmc\libraries"
$inst    = "$mmc\instances\clean vanilla 1.8.9"
$assets  = "$mmc\assets"
$natives = "$inst\natives"
$gameDir = "$inst\.minecraft"

# ── Extract native JARs once ──────────────────────────────────────────────────
$nativeJars = @(
    "$lib\net\java\jinput\jinput-platform\2.0.5\jinput-platform-2.0.5-natives-windows.jar",
    "$lib\org\lwjgl\lwjgl\lwjgl-platform\2.9.4-nightly-20150209\lwjgl-platform-2.9.4-nightly-20150209-natives-windows.jar",
    "$lib\tv\twitch\twitch-platform\6.5\twitch-platform-6.5-natives-windows-64.jar",
    "$lib\tv\twitch\twitch-external-platform\4.5\twitch-external-platform-4.5-natives-windows-64.jar"
)

if (-not (Test-Path $natives)) { New-Item -ItemType Directory -Path $natives -Force | Out-Null }

if (-not (Test-Path "$natives\lwjgl.dll")) {
    Write-Host "[*] Extracting native libraries..."
    Add-Type -Assembly System.IO.Compression.FileSystem
    foreach ($jar in $nativeJars) {
        if (Test-Path $jar) {
            try {
                $zip = [System.IO.Compression.ZipFile]::OpenRead($jar)
                foreach ($entry in $zip.Entries) {
                    if ($entry.Name -match '\.(dll|so|dylib)$') {
                        [System.IO.Compression.ZipFileExtensions]::ExtractToFile(
                            $entry, (Join-Path $natives $entry.Name), $true)
                    }
                }
                $zip.Dispose()
            } catch { Write-Warning "Could not extract $([System.IO.Path]::GetFileName($jar)): $_" }
        }
    }
    Write-Host "[OK] Natives extracted."
} else {
    Write-Host "[*] Natives already present."
}

# ── Classpath ─────────────────────────────────────────────────────────────────
$jars = @(
    "$mmc\jars\NewLaunch.jar",
    "$lib\net\java\jinput\jinput\2.0.5\jinput-2.0.5.jar",
    "$lib\net\java\jutils\jutils\1.0.0\jutils-1.0.0.jar",
    "$lib\org\lwjgl\lwjgl\lwjgl\2.9.4-nightly-20150209\lwjgl-2.9.4-nightly-20150209.jar",
    "$lib\org\lwjgl\lwjgl\lwjgl_util\2.9.4-nightly-20150209\lwjgl_util-2.9.4-nightly-20150209.jar",
    "$lib\com\mojang\netty\1.8.8\netty-1.8.8.jar",
    "$lib\oshi-project\oshi-core\1.1\oshi-core-1.1.jar",
    "$lib\net\java\dev\jna\jna\3.4.0\jna-3.4.0.jar",
    "$lib\net\java\dev\jna\platform\3.4.0\platform-3.4.0.jar",
    "$lib\com\ibm\icu\icu4j-core-mojang\51.2\icu4j-core-mojang-51.2.jar",
    "$lib\net\sf\jopt-simple\jopt-simple\4.6\jopt-simple-4.6.jar",
    "$lib\com\paulscode\codecjorbis\20101023\codecjorbis-20101023.jar",
    "$lib\com\paulscode\codecwav\20101023\codecwav-20101023.jar",
    "$lib\com\paulscode\libraryjavasound\20101123\libraryjavasound-20101123.jar",
    "$lib\com\paulscode\librarylwjglopenal\20100824\librarylwjglopenal-20100824.jar",
    "$lib\com\paulscode\soundsystem\20120107\soundsystem-20120107.jar",
    "$lib\io\netty\netty-all\4.0.23.Final\netty-all-4.0.23.Final.jar",
    "$lib\com\google\guava\guava\17.0\guava-17.0.jar",
    "$lib\org\apache\commons\commons-lang3\3.3.2\commons-lang3-3.3.2.jar",
    "$lib\commons-io\commons-io\2.4\commons-io-2.4.jar",
    "$lib\commons-codec\commons-codec\1.9\commons-codec-1.9.jar",
    "$lib\com\google\code\gson\gson\2.2.4\gson-2.2.4.jar",
    "$lib\com\mojang\authlib\1.5.21\authlib-1.5.21.jar",
    "$lib\com\mojang\realms\1.7.59\realms-1.7.59.jar",
    "$lib\org\apache\commons\commons-compress\1.8.1\commons-compress-1.8.1.jar",
    "$lib\org\apache\httpcomponents\httpclient\4.3.3\httpclient-4.3.3.jar",
    "$lib\commons-logging\commons-logging\1.1.3\commons-logging-1.1.3.jar",
    "$lib\org\apache\httpcomponents\httpcore\4.3.2\httpcore-4.3.2.jar",
    "$lib\org\apache\logging\log4j\log4j-api\2.0-beta9-fixed\log4j-api-2.0-beta9-fixed.jar",
    "$lib\org\apache\logging\log4j\log4j-core\2.0-beta9-fixed\log4j-core-2.0-beta9-fixed.jar",
    "$lib\tv\twitch\twitch\6.5\twitch-6.5.jar",
    "$lib\com\mojang\minecraft\1.8.9\minecraft-1.8.9-client.jar"
)
$cp = $jars -join ';'

# ── Write stdin protocol (ASCII, no BOM) ──────────────────────────────────────
$proto = "$env:TEMP\mc189_stdin.txt"
$ascii = [System.Text.Encoding]::ASCII

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("launcher onesix")
$lines.Add("cp $cp")
@(
    '--username', 'nonoooooooo',
    '--version',  '1.8.9',
    '--gameDir',  $gameDir,
    '--assetsDir', $assets,
    '--assetIndex', '1.8',
    '--uuid',     '00000000-0000-0000-0000-000000000001',
    '--accessToken', '0',
    '--userProperties', '{}',
    '--userType', 'legacy'
) | ForEach-Object { $lines.Add("param $_") }
$lines.Add("mainClass net.minecraft.client.main.Main")
$lines.Add("appletClass net.minecraft.client.MinecraftApplet")
$lines.Add("natives $natives")
$lines.Add("userName nonoooooooo")
$lines.Add("sessionId token:0:00000000-0000-0000-0000-000000000001")
$lines.Add("windowTitle Minecraft 1.8.9")
$lines.Add("windowParams 854x480")
$lines.Add("instanceTitle clean vanilla 1.8.9")
$lines.Add("instanceIconId default")
$lines.Add("launch")
[System.IO.File]::WriteAllLines($proto, [string[]]$lines, $ascii)

# ── Write launcher .bat ───────────────────────────────────────────────────────
$bat = "$env:TEMP\mc189_launch.bat"
$batContent  = "@echo off`r`n"
# start /min launches in a minimized window so the console doesn't intrude
$batContent += "start /min `"`" `"$java`" "
$batContent += "-XX:HeapDumpPath=MojangTricksIntelDriversForPerformance_javaw.exe_minecraft.exe.heapdump "
$batContent += "-Xms512m -Xmx1024m -Duser.language=en "
$batContent += "`"-Djava.library.path=$natives`" "
$batContent += "-cp `"$cp`" "
$batContent += "org.multimc.EntryPoint 0<`"$proto`"`r`n"
[System.IO.File]::WriteAllText($bat, $batContent, $ascii)

Write-Host "[*] Launching Minecraft 1.8.9 (offline)..."
Start-Process cmd -ArgumentList "/C `"$bat`""
Write-Host "[OK] Done. Wait for the Minecraft main menu before injecting."
