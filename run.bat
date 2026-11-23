@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title Qwen3-TTS (C++)

if not exist "bin\t2v-server.exe" (
  echo t2v-server.exe not found - running build.bat first...
  call build.bat --from-run || exit /b 1
)
if exist config.bat call config.bat
if "%QUANT%"=="" set "QUANT=Q8_0"
if "%PORT%"=="" set "PORT=7860"

set "PATH=%~dp0bin;%PATH%"
"bin\t2v-server.exe" ^
  --design "models\qwen-talker-1.7b-voicedesign-%QUANT%.gguf" ^
  --base   "models\qwen-talker-1.7b-base-%QUANT%.gguf" ^
  --codec  "models\qwen-tokenizer-12hz-F32.gguf" ^
  --web web --data . --port %PORT%
if errorlevel 1 pause
