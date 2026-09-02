[CmdletBinding()]
param(
    [ValidateSet('All', 'Engines', 'ScanEngine', 'VoiceEngine', 'Studio')]
    [string]$Target = 'All',
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$suiteRoot = Split-Path -Parent $PSScriptRoot

function Invoke-CMakePreset {
    param(
        [Parameter(Mandatory)] [string]$Repository,
        [Parameter(Mandatory)] [string]$Preset
    )

    $repositoryRoot = Join-Path $suiteRoot $Repository
    if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot 'CMakePresets.json'))) {
        throw "Missing or uninitialized repository: $Repository"
    }

    Push-Location $repositoryRoot
    try {
        & cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed for $Repository"
        }
        if (-not $ConfigureOnly) {
            & cmake --build --preset $Preset
            if ($LASTEXITCODE -ne 0) {
                throw "CMake build failed for $Repository"
            }
        }
    }
    finally {
        Pop-Location
    }
}

if ($Target -in @('All', 'Engines', 'ScanEngine')) {
    Invoke-CMakePreset -Repository 'engines/ScanEngine' -Preset 'win-qt5.12.9-msvc-mlx-cuda'
}

if ($Target -in @('All', 'Engines', 'VoiceEngine')) {
    Invoke-CMakePreset -Repository 'engines/VoiceEngine' -Preset 'win-qt5.12.9-msvc-cuda'
}

if ($Target -in @('All', 'Studio')) {
    $requiredSdkConfigs = @(
        'engines/ScanEngine/build/win-qt5.12.9-msvc-mlx-cuda/sdk/cmake/ScanEngineConfig.cmake',
        'engines/VoiceEngine/build/win-qt5.12.9-msvc-cuda/sdk/cmake/VoiceEngineConfig.cmake'
    )
    $missingSdkConfigs = @(
        foreach ($relativePath in $requiredSdkConfigs) {
            if (-not (Test-Path -LiteralPath (Join-Path $suiteRoot $relativePath))) { $relativePath }
        }
    )
    if ($missingSdkConfigs.Count -gt 0 -and $Target -eq 'All' -and $ConfigureOnly) {
        Write-Warning 'Skipping RecognitionStudio configure: configure-only does not create the engine SDK packages.'
    }
    elseif ($missingSdkConfigs.Count -gt 0) {
        throw "Build the engine SDKs first; missing $($missingSdkConfigs -join ', ')"
    }
    else {
        Invoke-CMakePreset -Repository 'apps/RecognitionStudio' -Preset 'windows-msvc-qt5'
    }
}
