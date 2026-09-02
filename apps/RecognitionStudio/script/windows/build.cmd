@echo off
setlocal
cd /d "%~dp0\..\.."
cmake --preset windows-msvc-qt5 || exit /b 1
cmake --build --preset windows-msvc-qt5 || exit /b 1
echo.
echo Built: build\windows-msvc-qt5\bin\RecognitionStudio.exe
