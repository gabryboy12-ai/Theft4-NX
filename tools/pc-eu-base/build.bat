@echo off
rem Windows build of the Switch game's generated code (see CMakeLists.txt).
rem Visual Studio's clang, CMake and Ninja, as for rexglue (06 section 6).
rem Rule (docs/switch-port/09-rules.md section 7): no other build running,
rem at most -j3 on the generated files.
rem
rem A clean PATH first: started from MSYS, its python3 (which cannot read
rem C:/ paths) would be the one glslang's configure finds.
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem;D:\Python"
call "D:\Microsoft Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=D:\Microsoft Visual Studio\VC\Tools\Llvm\x64\bin;D:\Microsoft Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;D:\Microsoft Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0..\.."
echo === CONFIGURE %TIME%
if not exist out\build\pc-eu-base\build.ninja (
  cmake -S tools/pc-eu-base -B out/build/pc-eu-base -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DPython3_EXECUTABLE=D:/Python/python.exe -DPYTHON_EXECUTABLE=D:/Python/python.exe || exit /b 2
)
echo === BUILD %TIME%
ninja -C out/build/pc-eu-base -j3 theft4-eu-base || exit /b 3
echo === DONE %TIME%
