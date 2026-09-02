@echo off
setlocal
if "%~1"=="" exit /b 2
set "source_dir=%~dp0..\..\models\qwen3-asr-1.7b"
set "destination_dir=%~1"
set "part1=%source_dir%\Qwen3-ASR-1.7B-bf16.gguf.part1"
set "part2=%source_dir%\Qwen3-ASR-1.7B-bf16.gguf.part2"
set "destination=%destination_dir%\Qwen3-ASR-1.7B-bf16.gguf"
if not exist "%part1%" exit /b 3
if not exist "%part2%" exit /b 3
if not exist "%destination_dir%" mkdir "%destination_dir%" || exit /b 4
copy /b /y "%part1%"+"%part2%" "%destination%" >nul || exit /b 5
for %%A in ("%part1%") do set "size1=%%~zA"
for %%A in ("%part2%") do set "size2=%%~zA"
for %%A in ("%destination%") do set "sizeout=%%~zA"
if "%sizeout%"=="" exit /b 6
echo Assembled Qwen3-ASR model: %size1% + %size2% = %sizeout% bytes
