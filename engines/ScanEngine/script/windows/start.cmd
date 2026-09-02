@echo off
setlocal EnableExtensions
cd /d "%~dp0"

call :init

if /i "%~1"=="" goto :menu
if /i "%~1"=="help" goto :help
if /i "%~1"=="check" goto :check
if /i "%~1"=="download" goto :download
if /i "%~1"=="install" goto :install
if /i "%~1"=="build" goto :build
if /i "%~1"=="clean-build" goto :clean_build
if /i "%~1"=="clean-offline" goto :clean_dl
echo Unknown command: %~1
goto :help

:menu
call :probe
echo.
echo ========== ScanEngine Windows ==========
echo repo:     %SCANENGINE_ROOT%
echo offline:  %OFFLINE_DIR%
echo.
echo Installed
echo   Qt 5.12.9      %ST_QT%
echo   CMake          %ST_CMAKE%
echo   VS 2022 v143   %ST_VS%
echo   CUDA 12.9      %ST_CUDA%
echo   cuDNN 9.9.0    %ST_CUDNN%
echo   MLX lib        %ST_MLX%
echo   models         %ST_MODEL%
echo.
echo Offline pack     %ST_OFFLINE%
echo Build output     %ST_BIN%
echo Desktop pack     %ST_PACK%
echo.
echo Next: %HINT%
echo.
echo   1  Download offline pack
echo   2  Install from offline pack
echo   3  Build and pack
echo   4  Check again
echo   5  Clean build dir
echo   6  Clean offline pack
echo   0  Exit
echo.
set "CHOICE="
set /p "CHOICE=Select: "
if "%CHOICE%"=="1" goto :download
if "%CHOICE%"=="2" goto :install
if "%CHOICE%"=="3" goto :build
if "%CHOICE%"=="4" goto :check
if "%CHOICE%"=="5" goto :clean_build
if "%CHOICE%"=="6" goto :clean_dl
if "%CHOICE%"=="0" goto :end
echo Invalid choice
goto :menu

:help
echo Usage:
echo   start.cmd
echo   start.cmd download
echo   start.cmd install
echo   start.cmd build [pack-dir]
echo   start.cmd check
echo   start.cmd clean-build
echo   start.cmd clean-offline
goto :end

:check
call :probe
echo Qt=%ST_QT%
echo CMake=%ST_CMAKE%
echo VS=%ST_VS%
echo CUDA=%ST_CUDA%
echo cuDNN=%ST_CUDNN%
echo MLX=%ST_MLX%
echo models=%ST_MODEL%
echo offline=%ST_OFFLINE%
echo build=%ST_BIN%
echo pack=%ST_PACK%
echo repo=%SCANENGINE_ROOT%
echo Next: %HINT%
if "%~1"=="" goto :menu
if "%READY%"=="1" goto :end
set "ERR=1"
goto :end

:download
echo ---- download into offline pack ----
if not exist "%OFFLINE_DIR%" mkdir "%OFFLINE_DIR%"
call :fetch "%URL_VS%" "%VS_BOOTSTRAP%" "VS 2022 Build Tools"
if errorlevel 1 goto :fail
call :fetch "%URL_CMAKE%" "%CMAKE_MSI%" "CMake 3.30.5"
if errorlevel 1 goto :fail
call :fetch "%URL_CUDA%" "%CUDA_EXE%" "CUDA Toolkit 12.9"
if errorlevel 1 goto :fail
call :fetch "%URL_CUDNN%" "%CUDNN_ZIP%" "cuDNN 9.9.0.52 CUDA12"
if errorlevel 1 goto :fail
call :fetch "%URL_QT%" "%QT_EXE%" "Qt 5.12.9"
if errorlevel 1 goto :fail
call :probe
echo offline pack: %OFFLINE_DIR%
echo status: %ST_OFFLINE%
if not "%HAS_DL_ALL%"=="1" (
    echo Offline pack is incomplete.
    goto :fail
)
echo Pack is complete. Copy the offline folder, then run install.
if "%~1"=="" goto :menu
goto :end

:install
echo ---- install from offline pack ----
call :probe
if "%HAS_VS%"=="1" (
    echo [skip] VS 2022 v143
) else (
    if not exist "%VS_BOOTSTRAP%" goto :need_dl
    echo installing VS 2022 Build Tools ...
    "%VS_BOOTSTRAP%" --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --includeRecommended
    if errorlevel 1 goto :fail
)
if "%HAS_CMAKE%"=="1" (
    echo [skip] CMake
) else (
    if not exist "%CMAKE_MSI%" goto :need_dl
    echo installing CMake 3.30.5 ...
    msiexec /i "%CMAKE_MSI%" /qn ADD_CMAKE_TO_PATH=System
    if errorlevel 1 (
        echo silent CMake install failed, opening installer
        start "" "%CMAKE_MSI%"
        pause
    )
    call :find_cmake
)
if "%HAS_CUDA%"=="1" (
    echo [skip] CUDA 12.9
) else (
    if not exist "%CUDA_EXE%" goto :need_dl
    echo installing CUDA 12.9 ...
    "%CUDA_EXE%" -s
    if errorlevel 1 (
        echo silent CUDA install failed, opening installer
        start "" "%CUDA_EXE%"
        pause
    )
)
if "%HAS_QT%"=="1" (
    echo [skip] Qt 5.12.9
) else (
    if not exist "%QT_EXE%" goto :need_dl
    echo installing Qt 5.12.9 - choose msvc2017 64-bit, root C:\Qt
    start /wait "" "%QT_EXE%"
    if not exist "%QT_ROOT%\bin\qmake.exe" (
        echo Qt not found at %QT_ROOT%
        goto :fail
    )
)
if "%HAS_CUDNN%"=="1" (
    echo [skip] cuDNN
) else (
    if not exist "%CUDNN_ZIP%" goto :need_dl
    call :install_cudnn
    if errorlevel 1 goto :fail
)
call :probe
if "%READY%"=="1" (
    echo Install done.
    if "%~1"=="" goto :menu
    goto :end
)
echo Install finished with gaps: %HINT%
if "%HAS_MLX%"=="0" echo   run: git lfs pull
if "%HAS_MODEL%"=="0" echo   run: git lfs pull
if "%~1"=="" goto :menu
set "ERR=1"
goto :end

:need_dl
echo Offline pack incomplete. Run download first.
if "%~1"=="" goto :menu
set "ERR=1"
goto :end

:fail
echo Failed.
if "%~1"=="" goto :menu
set "ERR=1"
goto :end

:build
call :probe
if "%READY%"=="0" (
    echo Environment not ready: %HINT%
    if "%~1"=="" goto :menu
    set "ERR=1"
    goto :end
)
if /i "%~1"=="build" if not "%~2"=="" set "PACK_DIR=%~2"
if not defined PACK_DIR set "PACK_DIR=%PACK_DEFAULT%"
echo configure %PRESET% ...
pushd "%SCANENGINE_ROOT%"
"%CMAKE_EXE%" --preset "%PRESET%"
if errorlevel 1 (
    popd
    goto :fail
)
echo build ...
"%CMAKE_EXE%" --build --preset "%PRESET%"
if errorlevel 1 (
    popd
    goto :fail
)
if not exist "%BIN_DIR%\ScanEngine.exe" (
    echo missing exe
    popd
    goto :fail
)
if not exist "%BIN_DIR%\runtimes\cuda\cublasLt64_12.dll" (
    echo missing CUDA runtime
    popd
    goto :fail
)
echo pack to %PACK_DIR% ...
if not exist "%PACK_DIR%" mkdir "%PACK_DIR%"
if exist "%PACK_DIR%\runtimes\cuda\cublas64_12.dll" del /f /q "%PACK_DIR%\runtimes\cuda\cublas64_12.dll"
if exist "%PACK_DIR%\runtimes\cuda\nvJitLink_120_0.dll" del /f /q "%PACK_DIR%\runtimes\cuda\nvJitLink_120_0.dll"
robocopy "%BIN_DIR%" "%PACK_DIR%" /E /NFL /NDL /NJH /NJS /nc /ns /np /XD output /XF .runtime.stamp .models.stamp .cuda-runtime.stamp
if errorlevel 8 (
    popd
    goto :fail
)
> "%PACK_DIR%\run.bat" (
    echo @echo off
    echo set MLX_CUDA_CONV_CACHE_SIZE=2048
    echo start "" "%%~dp0ScanEngine.exe"
)
popd
echo done: %PACK_DIR%\run.bat
if "%~1"=="" goto :menu
goto :end

:clean_build
if exist "%SCANENGINE_ROOT%\build" (
    echo removing %SCANENGINE_ROOT%\build
    rmdir /s /q "%SCANENGINE_ROOT%\build"
) else echo build dir not found
if "%~1"=="" goto :menu
goto :end

:clean_dl
if exist "%OFFLINE_DIR%" (
    echo removing %OFFLINE_DIR%
    rmdir /s /q "%OFFLINE_DIR%"
) else echo offline dir not found
if "%~1"=="" goto :menu
goto :end

:init
if not defined SystemRoot set "SystemRoot=C:\Windows"
if not defined SystemDrive set "SystemDrive=C:"
if not defined windir set "windir=C:\Windows"
if not defined USERPROFILE set "USERPROFILE=C:\Users\Administrator"
if not defined LOCALAPPDATA set "LOCALAPPDATA=%USERPROFILE%\AppData\Local"
if not defined APPDATA set "APPDATA=%USERPROFILE%\AppData\Roaming"
if not defined ProgramData set "ProgramData=C:\ProgramData"
if not defined TEMP set "TEMP=%LOCALAPPDATA%\Temp"
if not defined TMP set "TMP=%TEMP%"
set "SCANENGINE_ROOT=%~dp0..\.."
for %%I in ("%SCANENGINE_ROOT%") do set "SCANENGINE_ROOT=%%~fI"
set "PRESET=win-qt5.12.9-msvc-mlx-cuda"
set "BUILD_DIR=%SCANENGINE_ROOT%\build\%PRESET%"
set "BIN_DIR=%BUILD_DIR%\bin"
set "QT_ROOT=C:\Qt\Qt5.12.9\5.12.9\msvc2017_64"
set "CUDA_ROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9"
set "OFFLINE_DIR=%~dp0offline"
set "PACK_DEFAULT=%USERPROFILE%\Desktop\ScanEngineWindowsGpu"
set "VS_BOOTSTRAP=%OFFLINE_DIR%\vs_BuildTools.exe"
set "CMAKE_MSI=%OFFLINE_DIR%\cmake-3.30.5-windows-x86_64.msi"
set "CUDA_EXE=%OFFLINE_DIR%\cuda_12.9.0_576.02_windows.exe"
set "CUDNN_ZIP=%OFFLINE_DIR%\cudnn-windows-x86_64-9.9.0.52_cuda12-archive.zip"
set "QT_EXE=%OFFLINE_DIR%\qt-opensource-windows-x86-5.12.9.exe"
set "URL_VS=https://aka.ms/vs/17/release/vs_BuildTools.exe"
set "URL_CMAKE=https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-windows-x86_64.msi"
set "URL_CUDA=https://developer.download.nvidia.com/compute/cuda/12.9.0/local_installers/cuda_12.9.0_576.02_windows.exe"
set "URL_CUDNN=https://developer.download.nvidia.com/compute/cudnn/redist/cudnn/windows-x86_64/cudnn-windows-x86_64-9.9.0.52_cuda12-archive.zip"
set "URL_QT=https://download.qt.io/archive/qt/5.12/5.12.9/qt-opensource-windows-x86-5.12.9.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
call :find_cmake
goto :eof

:find_cmake
set "CMAKE_EXE="
if exist "C:\Qt\Tools\CMake_64\bin\cmake.exe" set "CMAKE_EXE=C:\Qt\Tools\CMake_64\bin\cmake.exe"
if not defined CMAKE_EXE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
if defined CMAKE_EXE goto :eof
where cmake >nul 2>nul
if errorlevel 1 goto :eof
for /f "delims=" %%I in ('where cmake') do (
    if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
)
goto :eof

:probe
set "HAS_QT=0"
set "ST_QT=missing"
set "HAS_CMAKE=0"
set "ST_CMAKE=missing"
set "HAS_VS=0"
set "ST_VS=missing"
set "HAS_CUDA=0"
set "ST_CUDA=missing"
set "HAS_CUDNN=0"
set "ST_CUDNN=missing"
set "HAS_MLX=0"
set "ST_MLX=missing"
set "HAS_MODEL=0"
set "ST_MODEL=missing"
set "HAS_DL_VS=0"
set "HAS_DL_CMAKE=0"
set "HAS_DL_CUDA=0"
set "HAS_DL_CUDNN=0"
set "HAS_DL_QT=0"
set "HAS_DL_ALL=0"
set "ST_OFFLINE=incomplete"
set "ST_BIN=none"
set "ST_PACK=none"
set "READY=0"
if exist "%QT_ROOT%\bin\qmake.exe" (
    set "HAS_QT=1"
    set "ST_QT=OK"
)
call :find_cmake
if defined CMAKE_EXE (
    set "HAS_CMAKE=1"
    set "ST_CMAKE=OK"
)
if exist "%VSWHERE%" (
    for /f "delims=" %%P in ('"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul') do (
        if not "%%P"=="" (
            set "HAS_VS=1"
            set "ST_VS=OK"
        )
    )
)
if exist "%CUDA_ROOT%\include\cuda.h" (
    set "HAS_CUDA=1"
    set "ST_CUDA=OK"
)
if exist "%CUDA_ROOT%\bin\cudnn64_9.dll" if exist "%CUDA_ROOT%\include\cudnn_version.h" (
    set "HAS_CUDNN=1"
    set "ST_CUDNN=OK"
)
if exist "%SCANENGINE_ROOT%\third_party\windows-x64\mlx\lib\mlx.lib" (
    set "HAS_MLX=1"
    set "ST_MLX=OK"
)
if exist "%SCANENGINE_ROOT%\models\vlm\model.safetensors.part1" if exist "%SCANENGINE_ROOT%\models\vlm\model.safetensors.part2" (
    set "HAS_MODEL=1"
    set "ST_MODEL=OK"
)
if exist "%BIN_DIR%\ScanEngine.exe" set "ST_BIN=OK"
if exist "%PACK_DEFAULT%\ScanEngine.exe" set "ST_PACK=OK"
if exist "%VS_BOOTSTRAP%" set "HAS_DL_VS=1"
if exist "%CMAKE_MSI%" set "HAS_DL_CMAKE=1"
if exist "%CUDA_EXE%" set "HAS_DL_CUDA=1"
if exist "%CUDNN_ZIP%" set "HAS_DL_CUDNN=1"
if exist "%QT_EXE%" set "HAS_DL_QT=1"
if "%HAS_DL_VS%"=="1" if "%HAS_DL_CMAKE%"=="1" if "%HAS_DL_CUDA%"=="1" if "%HAS_DL_CUDNN%"=="1" if "%HAS_DL_QT%"=="1" (
    set "HAS_DL_ALL=1"
    set "ST_OFFLINE=complete"
)
set "HINT=ready to build"
if "%HAS_MODEL%"=="0" set "HINT=missing models, run git lfs pull"
if "%HAS_MLX%"=="0" set "HINT=missing mlx.lib, run git lfs pull"
if "%HAS_QT%"=="0" set "HINT=missing Qt, download then install"
if "%HAS_CUDNN%"=="0" set "HINT=missing cuDNN, download then install"
if "%HAS_CUDA%"=="0" set "HINT=missing CUDA, download then install"
if "%HAS_CMAKE%"=="0" set "HINT=missing CMake, download then install"
if "%HAS_VS%"=="0" set "HINT=missing VS 2022, download then install"
if "%READY%"=="0" if "%HAS_DL_ALL%"=="0" set "HINT=offline pack incomplete, choose 1"
if "%READY%"=="0" if "%HAS_DL_ALL%"=="1" set "HINT=offline pack ready, choose 2"
if "%HAS_QT%"=="1" if "%HAS_CMAKE%"=="1" if "%HAS_VS%"=="1" if "%HAS_CUDA%"=="1" if "%HAS_CUDNN%"=="1" if "%HAS_MLX%"=="1" if "%HAS_MODEL%"=="1" (
    set "READY=1"
    set "HINT=environment ready, choose 3 to build"
)
goto :eof

:install_cudnn
if not exist "%CUDA_ROOT%\include\cuda.h" (
    echo CUDA root missing
    exit /b 1
)
echo extracting cuDNN into CUDA 12.9 ...
if exist "%OFFLINE_DIR%\cudnn_unpack" rmdir /s /q "%OFFLINE_DIR%\cudnn_unpack"
mkdir "%OFFLINE_DIR%\cudnn_unpack"
tar.exe -xf "%CUDNN_ZIP%" -C "%OFFLINE_DIR%\cudnn_unpack"
if errorlevel 1 exit /b 1
set "CUDNN_SRC="
for /d %%D in ("%OFFLINE_DIR%\cudnn_unpack\*") do set "CUDNN_SRC=%%~fD"
if not defined CUDNN_SRC exit /b 1
if exist "%CUDNN_SRC%\bin" robocopy "%CUDNN_SRC%\bin" "%CUDA_ROOT%\bin" /E /NFL /NDL /NJH /NJS /nc /ns /np
if exist "%CUDNN_SRC%\include" robocopy "%CUDNN_SRC%\include" "%CUDA_ROOT%\include" /E /NFL /NDL /NJH /NJS /nc /ns /np
if exist "%CUDNN_SRC%\lib\x64" (
    robocopy "%CUDNN_SRC%\lib\x64" "%CUDA_ROOT%\lib\x64" /E /NFL /NDL /NJH /NJS /nc /ns /np
) else if exist "%CUDNN_SRC%\lib" (
    robocopy "%CUDNN_SRC%\lib" "%CUDA_ROOT%\lib\x64" /E /NFL /NDL /NJH /NJS /nc /ns /np
)
if not exist "%CUDA_ROOT%\bin\cudnn64_9.dll" (
    echo cudnn64_9.dll not found after extract
    exit /b 1
)
echo [ok] cuDNN installed
exit /b 0

:fetch
if exist "%~2" (
    echo [skip] %~3
    exit /b 0
)
echo [download] %~3
curl.exe -fL --retry 3 --retry-delay 2 -o "%~2.part" "%~1"
if errorlevel 1 (
    echo curl failed, retrying with BITS ...
    if exist "%~2.part" del /f /q "%~2.part"
    bitsadmin /transfer "ScanEngine-%RANDOM%" /download /priority foreground "%~1" "%~2"
    if errorlevel 1 (
        echo download failed: %~3
        if exist "%~2" del /f /q "%~2"
        exit /b 1
    )
    echo [ok] %~3
    exit /b 0
)
if "%HAS_VS%"=="0" if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "HAS_VS=1"
    set "ST_VS=OK (Professional)"
)
move /y "%~2.part" "%~2" >nul
echo [ok] %~3
exit /b 0

:end
if "%~1"=="" pause
if defined ERR exit /b 1
exit /b 0
