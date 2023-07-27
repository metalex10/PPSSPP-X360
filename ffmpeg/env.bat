set SPEECHSDK=%PROGRAMFILES%\Microsoft SDKs\Speech\v11.0\Tools
if "%PROCESSOR_ARCHITECTURE%"=="AMD64" set SPEECHSDK=%PROGRAMFILES(x86)%\Microsoft SDKs\Speech\v11.0\Tools

set PATH=%XEDK%\bin\win32;%SPEECHSDK%;%PATH%;
set INCLUDE=%XEDK%\include\win32;%XEDK%\include\xbox;%XEDK%\include\xbox\sys;%INCLUDE%
set LIB=%XEDK%\lib\win32;%XEDK%\lib\xbox;%LIB%
set _NT_SYMBOL_PATH=SRV*%XEDK%\bin\xbox\symsrv;%_NT_SYMBOL_PATH%

cd /d "%XEDK%\bin\win32"

C:\MinGW\msys\1.0\msys.bat
