@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title Qwen3-TTS (C++) - build

rem ======================================================================
rem  build.bat : get qwentts.cpp, compile t2v-server.exe with CUDA,
rem              download the GGUF models. Safe to run again any time.
rem
rem  Optional environment overrides (set before running):
rem    QUANT       Q8_0 (default) / BF16 / Q4_K_M   talker precision
rem    CUDA_ARCH   native (default) or e.g. 89      GPU architecture
rem    QWENTTS_REF commit/branch of qwentts.cpp     (default: tested commit)
rem ======================================================================

if "%QWENTTS_REF%"=="" set "QWENTTS_REF=2edb35f87b709a070b2d5379f77e5c52a1daa585"
if "%QUANT%"=="" set "QUANT=Q8_0"
if "%CUDA_ARCH%"=="" set "CUDA_ARCH=native"
set "REPO=https://github.com/ServeurpersoCom/qwentts.cpp.git"
set "HF=https://huggingface.co/Serveurperso/Qwen3-TTS-GGUF/resolve/main"
set "BUILD=qwentts.cpp\build-t2v"

echo.
echo === [1/5] Checking tools ===
set "MISSING="
where git >nul 2>&1 || set "MISSING=%MISSING% git"
where curl >nul 2>&1 || set "MISSING=%MISSING% curl"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR set "MISSING=%MISSING% vs"

set "NVCC="
if defined CUDA_PATH if exist "%CUDA_PATH%\bin\nvcc.exe" set "NVCC=%CUDA_PATH%\bin\nvcc.exe"
if not defined NVCC for /f "delims=" %%i in ('where nvcc 2^>nul') do if not defined NVCC set "NVCC=%%i"
if not defined NVCC set "MISSING=%MISSING% cuda"

if not defined MISSING goto tools_ok
echo.
echo Missing:%MISSING%
echo   git  : winget install --id Git.Git -e
echo   vs   : Visual Studio 2022 Build Tools with "Desktop development with C++"
echo   cuda : NVIDIA CUDA Toolkit  https://developer.nvidia.com/cuda-downloads
echo.
where winget >nul 2>&1 || goto tools_fail
choice /c YN /m "Install the missing tools now with winget (needs admin, VS+CUDA are several GB)"
if errorlevel 2 goto tools_fail
echo %MISSING% | find "git" >nul && winget install --id Git.Git -e --accept-package-agreements --accept-source-agreements
echo %MISSING% | find "vs" >nul && winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
echo %MISSING% | find "cuda" >nul && winget install --id Nvidia.CUDA -e --accept-package-agreements --accept-source-agreements
echo.
echo Install finished. CLOSE this window and run build.bat again (PATH must refresh).
pause
exit /b 0
:tools_fail
echo Please install the tools above, then run build.bat again.
pause
exit /b 1

:tools_ok
for %%i in ("%NVCC%") do set "CUDA_BIN=%%~dpi"
echo   Visual Studio : %VSDIR%
echo   nvcc          : %NVCC%

echo.
echo === [2/5] Getting qwentts.cpp (%QWENTTS_REF%) ===
if not exist "qwentts.cpp\.git" (
  git clone --recurse-submodules "%REPO%" qwentts.cpp || goto fail
)
git -C qwentts.cpp checkout --quiet -- CMakeLists.txt 2>nul
git -C qwentts.cpp fetch --quiet origin || echo   [warn] fetch failed, using local copy
git -C qwentts.cpp checkout --quiet "%QWENTTS_REF%" || goto fail
git -C qwentts.cpp submodule update --init --recursive --quiet || goto fail
rem hook our server target into the qwentts.cpp CMake project
>>qwentts.cpp\CMakeLists.txt echo.
>>qwentts.cpp\CMakeLists.txt echo include("${CMAKE_SOURCE_DIR}/../server/t2v.cmake")

echo.
echo === [3/5] Compiling t2v-server.exe (CUDA, first time takes 5-20 min) ===
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto fail
where cmake >nul 2>&1 || (echo   cmake not found: winget install Kitware.CMake & goto fail)
where ninja >nul 2>&1 || (echo   ninja not found: winget install Ninja-build.Ninja & goto fail)
rem CMake needs forward slashes; --fresh drops any stale cache from a failed run
set "NVCC_CM=%NVCC:\=/%"
cmake --fresh -S qwentts.cpp -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGGML_CUDA=ON "-DCMAKE_CUDA_COMPILER=%NVCC_CM%" "-DCMAKE_CUDA_ARCHITECTURES=%CUDA_ARCH%" "-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH" || goto fail
cmake --build "%BUILD%" --target t2v-server -j %NUMBER_OF_PROCESSORS% || goto fail

echo.
echo === [4/5] Copying exe and CUDA runtime DLLs to bin\ ===
if not exist bin mkdir bin
copy /y "%BUILD%\t2v-server.exe" bin\ >nul || goto fail
for %%p in (cudart64_ cublas64_ cublasLt64_) do (
  copy /y "%CUDA_BIN%%%p*.dll" bin\ >nul 2>&1
  copy /y "%CUDA_BIN%x64\%%p*.dll" bin\ >nul 2>&1
)
dir /b bin

echo.
echo === [5/5] Downloading models (%QUANT%) ===
if not exist models mkdir models
call :dl qwen-talker-1.7b-voicedesign-%QUANT%.gguf || goto fail
call :dl qwen-talker-1.7b-base-%QUANT%.gguf || goto fail
call :dl qwen-tokenizer-12hz-F32.gguf || goto fail
>config.bat echo set "QUANT=%QUANT%"

echo.
echo ==============================================================
echo  Build finished. Double-click run.bat to start.
echo ==============================================================
if /i "%~1"=="--from-run" exit /b 0
pause
exit /b 0

:dl
if exist "models\%~1" (
  echo   [ok] %~1
  exit /b 0
)
echo   downloading %~1 ...
curl -L --fail --retry 3 -C - -o "models\%~1.part" "%HF%/%~1?download=true" || exit /b 1
move /y "models\%~1.part" "models\%~1" >nul
exit /b 0

:fail
echo.
echo *** Build failed. Scroll up for the first error message. ***
pause
exit /b 1
