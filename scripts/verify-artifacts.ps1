[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$suiteRoot = Split-Path -Parent $PSScriptRoot
$scanSdk = Join-Path $suiteRoot 'engines/ScanEngine/build/win-qt5.12.9-msvc-mlx-cuda/sdk'
$voiceSdk = Join-Path $suiteRoot 'engines/VoiceEngine/build/win-qt5.12.9-msvc-cuda/sdk'
$studioBin = Join-Path $suiteRoot 'apps/RecognitionStudio/build/windows-msvc-qt5/bin'
$failures = [System.Collections.Generic.List[string]]::new()

function Require-Path {
    param([Parameter(Mandatory)] [string]$Root, [Parameter(Mandatory)] [string]$RelativePath)
    $path = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("Missing: $path")
    }
}

function Forbid-Path {
    param([Parameter(Mandatory)] [string]$Root, [Parameter(Mandatory)] [string]$RelativePath)
    $path = Join-Path $Root $RelativePath
    if (Test-Path -LiteralPath $path) {
        $failures.Add("Unexpected development or repository path: $path")
    }
}

function Require-MatchingFile {
    param([Parameter(Mandatory)] [string]$Root, [Parameter(Mandatory)] [string]$Filter)
    if (-not (Test-Path -LiteralPath $Root) -or
        -not (Get-ChildItem -LiteralPath $Root -Filter $Filter -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        $failures.Add("Missing $Filter under: $Root")
    }
}

$scanSdkPaths = @(
    'bin/ScanEngineCore.dll', 'bin/scanengine.json',
    'bin/include/cccl', 'bin/include/cuda',
    'include/scanengine/scanengine_api.h', 'include/scanengine/scanengine.hpp',
    'include/scanengine/scanengine_runtime.h', 'include/scanengine/scanengine_version.h',
    'lib/ScanEngineCore.lib', 'lib/ScanEngineRuntime.lib',
    'cmake/ScanEngineConfig.cmake', 'doc/SDK_ABI.md', 'licenses'
)
foreach ($path in $scanSdkPaths) { Require-Path -Root $scanSdk -RelativePath $path }
Require-MatchingFile -Root (Join-Path $scanSdk 'bin/models') -Filter '*.safetensors*'
Require-MatchingFile -Root (Join-Path $scanSdk 'bin/runtimes/cuda') -Filter '*.dll'

$voiceSdkPaths = @(
    'bin/VoiceEngineCore.dll', 'bin/voiceengine.json',
    'include/voiceengine/voiceengine_api.h', 'include/voiceengine/voiceengine.hpp',
    'include/voiceengine/voiceengine_runtime.h', 'include/voiceengine/voiceengine_version.h',
    'lib/VoiceEngineCore.lib', 'lib/VoiceEngineRuntime.lib',
    'cmake/VoiceEngineConfig.cmake', 'doc/SDK_ABI.md', 'licenses'
)
foreach ($path in $voiceSdkPaths) { Require-Path -Root $voiceSdk -RelativePath $path }
Require-MatchingFile -Root (Join-Path $voiceSdk 'bin/models') -Filter '*.gguf'
Require-MatchingFile -Root (Join-Path $voiceSdk 'bin/runtimes/cuda') -Filter '*.dll'
Require-MatchingFile -Root (Join-Path $voiceSdk 'bin/runtimes/ffmpeg') -Filter '*.dll'

$studioPaths = @(
    'RecognitionStudio.exe',
    'components/scanengine/ScanEngineCore.dll',
    'components/scanengine/include/cccl',
    'components/scanengine/include/cuda',
    'components/voiceengine/VoiceEngineCore.dll',
    'licenses/scanengine', 'licenses/voiceengine', 'output'
)
foreach ($path in $studioPaths) { Require-Path -Root $studioBin -RelativePath $path }

$forbiddenStudioPaths = @(
    '.git', 'include', 'lib', 'cmake', 'examples', 'testdata',
    'components/scanengine/.git',
    'components/scanengine/include/scanengine',
    'components/scanengine/lib',
    'components/scanengine/cmake',
    'components/scanengine/examples',
    'components/voiceengine/.git',
    'components/voiceengine/include',
    'components/voiceengine/lib',
    'components/voiceengine/cmake',
    'components/voiceengine/examples'
)
foreach ($path in $forbiddenStudioPaths) { Forbid-Path -Root $studioBin -RelativePath $path }

if (Test-Path -LiteralPath $studioBin) {
    $legacyFiles = Get-ChildItem -LiteralPath $studioBin -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '(?i)(sfyzydoc|speechflow)' }
    foreach ($legacyFile in $legacyFiles) {
        $failures.Add("Legacy product name in artifact: $($legacyFile.FullName)")
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    exit 1
}

Write-Output 'Artifact layout OK: ScanEngine SDK, VoiceEngine SDK, and RecognitionStudio runtime.'
