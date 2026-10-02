@echo off
rem Windows build of the Switch game's generated code (see CMakeLists.txt).
rem Visual Studio's clang, CMake and Ninja, as for rexglue (06 section 6).
rem Rule (docs/switch-port/09-rules.md section 7): no other build running,
rem at most -j3 on the generated files.
rem
rem Two environment variables, set once per machine:
rem   THEFT4_VS_ROOT  the Visual Studio installation folder (the one with
rem                   VC\Auxiliary\Build\vcvars64.bat), e.g.
rem                   C:\Program Files\Microsoft Visual Studio\2022\Community
rem   THEFT4_PYTHON   a Windows Python (python.exe), not MSYS's
rem
rem A clean PATH first: started from MSYS, its python3 (which cannot read
rem C:/ paths) would be the one glslang's configure finds.
if not defined THEFT4_VS_ROOT (
  echo build.bat: set THEFT4_VS_ROOT to the Visual Studio folder that holds VC\Auxiliary\Build\vcvars64.bat
  exit /b 1
)
if not exist "%THEFT4_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" (
  echo build.bat: "%THEFT4_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" not found: check THEFT4_VS_ROOT
  exit /b 1
)
if not defined THEFT4_PYTHON (
  echo build.bat: set THEFT4_PYTHON to a Windows python.exe, not MSYS's python3
  exit /b 1
)
if not exist "%THEFT4_PYTHON%" (
  echo build.bat: "%THEFT4_PYTHON%" not found: check THEFT4_PYTHON
  exit /b 1
)
for %%P in ("%THEFT4_PYTHON%") do set "THEFT4_PYTHON_DIR=%%~dpP"
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem;%THEFT4_PYTHON_DIR%"
call "%THEFT4_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%THEFT4_VS_ROOT%\VC\Tools\Llvm\x64\bin;%THEFT4_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%THEFT4_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "THEFT4_PYTHON_CMAKE=%THEFT4_PYTHON:\=/%"
cd /d "%~dp0..\.."
echo === CONFIGURE %TIME%
if not exist out\build\pc-eu-base\build.ninja (
  cmake -S tools/pc-eu-base -B out/build/pc-eu-base -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_SCAN_FOR_MODULES=OFF "-DPython3_EXECUTABLE=%THEFT4_PYTHON_CMAKE%" "-DPYTHON_EXECUTABLE=%THEFT4_PYTHON_CMAKE%" || exit /b 2
)
echo === BUILD %TIME%
ninja -C out/build/pc-eu-base -j3 theft4-eu-base || exit /b 3
echo === DONE %TIME%
