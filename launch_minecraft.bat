@echo off
setlocal

:: ── Paths ────────────────────────────────────────────────────────────────────
set JAVA="C:\Program Files\Eclipse Adoptium\jdk-21.0.10.7-hotspot\bin\javaw.exe"
set MMC_LIB=C:\Program Files\mmc-develop-win32\MultiMC\libraries
set INST=C:\Program Files\mmc-develop-win32\MultiMC\instances\clean vanilla 1.8.9
set ASSETS=C:\Program Files\mmc-develop-win32\MultiMC\assets
set NATIVES=%INST%\natives
set GAMEDIR=%INST%\.minecraft

:: ── Classpath ─────────────────────────────────────────────────────────────────
set CP=
set CP=%CP%;%MMC_LIB%\net\java\jinput\jinput\2.0.5\jinput-2.0.5.jar
set CP=%CP%;%MMC_LIB%\net\java\jutils\jutils\1.0.0\jutils-1.0.0.jar
set CP=%CP%;%MMC_LIB%\org\lwjgl\lwjgl\lwjgl\2.9.4-nightly-20150209\lwjgl-2.9.4-nightly-20150209.jar
set CP=%CP%;%MMC_LIB%\org\lwjgl\lwjgl\lwjgl_util\2.9.4-nightly-20150209\lwjgl_util-2.9.4-nightly-20150209.jar
set CP=%CP%;%MMC_LIB%\com\mojang\netty\1.8.8\netty-1.8.8.jar
set CP=%CP%;%MMC_LIB%\oshi-project\oshi-core\1.1\oshi-core-1.1.jar
set CP=%CP%;%MMC_LIB%\net\java\dev\jna\jna\3.4.0\jna-3.4.0.jar
set CP=%CP%;%MMC_LIB%\net\java\dev\jna\platform\3.4.0\platform-3.4.0.jar
set CP=%CP%;%MMC_LIB%\com\ibm\icu\icu4j-core-mojang\51.2\icu4j-core-mojang-51.2.jar
set CP=%CP%;%MMC_LIB%\net\sf\jopt-simple\jopt-simple\4.6\jopt-simple-4.6.jar
set CP=%CP%;%MMC_LIB%\com\paulscode\codecjorbis\20101023\codecjorbis-20101023.jar
set CP=%CP%;%MMC_LIB%\com\paulscode\codecwav\20101023\codecwav-20101023.jar
set CP=%CP%;%MMC_LIB%\com\paulscode\libraryjavasound\20101123\libraryjavasound-20101123.jar
set CP=%CP%;%MMC_LIB%\com\paulscode\librarylwjglopenal\20100824\librarylwjglopenal-20100824.jar
set CP=%CP%;%MMC_LIB%\com\paulscode\soundsystem\20120107\soundsystem-20120107.jar
set CP=%CP%;%MMC_LIB%\io\netty\netty-all\4.0.23.Final\netty-all-4.0.23.Final.jar
set CP=%CP%;%MMC_LIB%\com\google\guava\guava\17.0\guava-17.0.jar
set CP=%CP%;%MMC_LIB%\org\apache\commons\commons-lang3\3.3.2\commons-lang3-3.3.2.jar
set CP=%CP%;%MMC_LIB%\commons-io\commons-io\2.4\commons-io-2.4.jar
set CP=%CP%;%MMC_LIB%\commons-codec\commons-codec\1.9\commons-codec-1.9.jar
set CP=%CP%;%MMC_LIB%\com\google\code\gson\gson\2.2.4\gson-2.2.4.jar
set CP=%CP%;%MMC_LIB%\com\mojang\authlib\1.5.21\authlib-1.5.21.jar
set CP=%CP%;%MMC_LIB%\com\mojang\realms\1.7.59\realms-1.7.59.jar
set CP=%CP%;%MMC_LIB%\org\apache\commons\commons-compress\1.8.1\commons-compress-1.8.1.jar
set CP=%CP%;%MMC_LIB%\org\apache\httpcomponents\httpclient\4.3.3\httpclient-4.3.3.jar
set CP=%CP%;%MMC_LIB%\commons-logging\commons-logging\1.1.3\commons-logging-1.1.3.jar
set CP=%CP%;%MMC_LIB%\org\apache\httpcomponents\httpcore\4.3.2\httpcore-4.3.2.jar
set CP=%CP%;%MMC_LIB%\org\apache\logging\log4j\log4j-api\2.0-beta9-fixed\log4j-api-2.0-beta9-fixed.jar
set CP=%CP%;%MMC_LIB%\org\apache\logging\log4j\log4j-core\2.0-beta9-fixed\log4j-core-2.0-beta9-fixed.jar
set CP=%CP%;%MMC_LIB%\tv\twitch\twitch\6.5\twitch-6.5.jar
set CP=%CP%;%MMC_LIB%\com\mojang\minecraft\1.8.9\minecraft-1.8.9-client.jar
:: strip leading semicolon
set CP=%CP:~1%

:: ── Launch ───────────────────────────────────────────────────────────────────
echo [*] Launching Minecraft 1.8.9 (offline)...

start "" %JAVA% ^
  -XX:HeapDumpPath=MojangTricksIntelDriversForPerformance_javaw.exe_minecraft.exe.heapdump ^
  -Xms512m -Xmx1024m ^
  -Duser.language=en ^
  -Djava.library.path="%NATIVES%" ^
  -cp "%CP%" ^
  net.minecraft.client.main.Main ^
  --username nonoooooooo ^
  --version 1.8.9 ^
  --gameDir "%GAMEDIR%" ^
  --assetsDir "%ASSETS%" ^
  --assetIndex 1.8 ^
  --uuid 00000000-0000-0000-0000-000000000001 ^
  --accessToken 0 ^
  --userProperties {} ^
  --userType legacy ^
  --width 854 --height 480

echo [*] Minecraft launched. Wait for the main menu before injecting.
endlocal
