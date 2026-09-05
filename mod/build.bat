@echo off
rem Master Looter build: MSVC Build Tools 2022 + the CMake and Ninja they bundle.
rem   build.bat          configure (first run) and build Release into build\, stage dist\
rem   build.bat clean    wipe build\ first
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "HERE=%~dp0"

if "%1"=="clean" if exist "%HERE%build" rmdir /s /q "%HERE%build"

call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
  echo vcvars64.bat not found at "%VCVARS%"
  exit /b 1
)

if not exist "%HERE%build\build.ninja" (
  "%CMAKE%" -S "%HERE%." -B "%HERE%build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release
  if errorlevel 1 exit /b 1
)
"%CMAKE%" --build "%HERE%build"
if errorlevel 1 exit /b 1
echo.
echo staged in "%HERE%dist"
endlocal
