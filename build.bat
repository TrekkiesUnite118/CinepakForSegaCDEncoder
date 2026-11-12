@echo off
rem Builds scdcpk.exe with MSVC (run from any prompt; finds vcvars64 if needed).
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
    if exist "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
    ) else (
        for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
)
if not exist obj mkdir obj
set INC=/Isrc /Ithird_party\tilepalquant
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS %INC% /Foobj\ /c src\main.c src\cin.c src\audio.c src\input.c src\vq.c src\par.c || exit /b 1
cl /nologo /O2 /W3 /EHsc /std:c++14 /D_CRT_SECURE_NO_WARNINGS %INC% /Foobj\ /c src\tpq.cpp third_party\tilepalquant\quant.cpp || exit /b 1
link /nologo /OUT:scdcpk.exe obj\*.obj || exit /b 1
echo built scdcpk.exe
