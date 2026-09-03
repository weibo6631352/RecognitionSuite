[CmdletBinding()]
param(
    [switch]$VerifySdkHashes,
    [switch]$VerifyCudaArchitectures,
    [switch]$SdkOnly,
    [switch]$SkipSdkGitTracking
)

$ErrorActionPreference = 'Stop'
$suiteRoot = Split-Path -Parent $PSScriptRoot
$scanSdk = Join-Path $suiteRoot 'sdk/ScanEngine/windows-x64'
$voiceSdk = Join-Path $suiteRoot 'sdk/VoiceEngine/windows-x64'
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

function Verify-SdkManifest {
    param([Parameter(Mandatory)] [string]$Root)

    $manifestPath = Join-Path $Root 'SDK_MANIFEST.json'
    $checksumsPath = Join-Path $Root 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $checksumsPath -PathType Leaf)) {
        $failures.Add("Missing SDK manifest or checksums under: $Root")
        return
    }

    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Encoding UTF8 -Raw | ConvertFrom-Json
    }
    catch {
        $failures.Add("Invalid SDK manifest: $manifestPath ($($_.Exception.Message))")
        return
    }

    if ($manifest.source_commit -notmatch '^[0-9a-fA-F]{40,64}$' -or
        $manifest.source_tree -notmatch '^[0-9a-fA-F]{40,64}$') {
        $failures.Add("SDK manifest has no valid source commit/tree identity: $manifestPath")
    }
    if ([int]$manifest.schema_version -lt 2 -or
        $manifest.cuda_toolkit_version -ne '12.9.41' -or
        $manifest.minimum_nvidia_driver_windows -ne '576.02') {
        $failures.Add("SDK manifest has no valid CUDA 12.9 compatibility identity: $manifestPath")
    }
    $nativeArchitectures = @($manifest.cuda_native_architectures)
    if ('sm_89' -notin $nativeArchitectures -or 'sm_120a' -notin $nativeArchitectures) {
        $failures.Add("SDK manifest does not declare RTX 40/50 native CUDA targets: $manifestPath")
    }
    $ptxArchitectures = @($manifest.cuda_ptx_architectures)
    if ('sm_89' -notin $ptxArchitectures -or
        (@('sm_120', 'sm_120a') | Where-Object { $_ -in $ptxArchitectures }).Count -eq 0) {
        $failures.Add("SDK manifest does not declare RTX 40/50 PTX targets: $manifestPath")
    }
    $gpuFamilies = @($manifest.gpu_families)
    if ('NVIDIA GeForce RTX 40' -notin $gpuFamilies -or
        'NVIDIA GeForce RTX 50' -notin $gpuFamilies) {
        $failures.Add("SDK manifest does not declare both supported GPU families: $manifestPath")
    }

    if (-not $SkipSdkGitTracking) {
        $enginePath = switch ($manifest.engine) {
            'ScanEngine' { 'engines/ScanEngine' }
            'VoiceEngine' { 'engines/VoiceEngine' }
            default { $null }
        }
        if (-not $enginePath) {
            $failures.Add("SDK manifest has an unknown engine identity: $manifestPath")
        }
        else {
            $indexTree = @(& git -C $suiteRoot write-tree)
            if ($LASTEXITCODE -ne 0 -or $indexTree.Count -ne 1) {
                $failures.Add("Unable to resolve the staged source tree for: $manifestPath")
            }
            else {
                $expectedSourceTree = @(
                    & git -C $suiteRoot rev-parse "$($indexTree[0])`:$enginePath"
                )
                if ($LASTEXITCODE -ne 0 -or $expectedSourceTree.Count -ne 1 -or
                    $manifest.source_tree -ne $expectedSourceTree[0]) {
                    $failures.Add("SDK source tree does not match the staged engine tree: $manifestPath")
                }
            }
        }
    }

    $actualChecksumsHash = (Get-FileHash -LiteralPath $checksumsPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualChecksumsHash -ne $manifest.checksums_sha256) {
        $failures.Add("SDK checksum manifest hash mismatch: $checksumsPath")
    }

    $suitePrefix = [System.IO.Path]::GetFullPath($suiteRoot).TrimEnd('\') + '\'
    $rootFullPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    if (-not $rootFullPath.StartsWith($suitePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        $failures.Add("SDK root is outside the repository: $Root")
        return
    }
    $rootRelative = $rootFullPath.Substring($suitePrefix.Length).Replace('\', '/')
    $trackedSet = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    if (-not $SkipSdkGitTracking) {
        $trackedFiles = @(& git -C $suiteRoot ls-files -- $rootRelative)
        if ($LASTEXITCODE -ne 0) {
            $failures.Add("Unable to inspect tracked SDK files under: $Root")
            return
        }
        foreach ($trackedFile in $trackedFiles) {
            [void]$trackedSet.Add($trackedFile.Replace('\', '/'))
        }
        $expectedTrackedCount = [int]$manifest.payload_file_count + 2
        if ($trackedSet.Count -ne $expectedTrackedCount) {
            $failures.Add("SDK tracked-file count mismatch under $Root; expected $expectedTrackedCount, got $($trackedSet.Count)")
        }
        foreach ($metadataName in @('SDK_MANIFEST.json', 'SHA256SUMS.txt')) {
            if (-not $trackedSet.Contains("$rootRelative/$metadataName")) {
                $failures.Add("SDK metadata is not tracked by Git: $rootRelative/$metadataName")
            }
        }
    }
    $manifestTextAttribute = if ($SkipSdkGitTracking) {
        @(& git -C $suiteRoot check-attr text -- "$rootRelative/SDK_MANIFEST.json")
    }
    else {
        @(& git -C $suiteRoot check-attr --cached text -- "$rootRelative/SDK_MANIFEST.json")
    }
    if ($LASTEXITCODE -ne 0 -or $manifestTextAttribute -notmatch ': text: unset$') {
        $failures.Add("SDK byte preservation rule is missing for: $rootRelative")
    }

    $payloadFiles = @(
        Get-ChildItem -LiteralPath $Root -File -Recurse -Force |
            Where-Object { $_.FullName -notin @($manifestPath, $checksumsPath) }
    )
    foreach ($oversizedFile in @($payloadFiles | Where-Object { $_.Length -ge 2GB })) {
        $failures.Add("SDK payload reaches or exceeds the 2 GiB remote limit: $($oversizedFile.FullName)")
    }
    $payloadByPath = @{}
    foreach ($payloadFile in $payloadFiles) {
        $relativePath = $payloadFile.FullName.Substring($rootFullPath.Length + 1).Replace('\', '/')
        $payloadByPath[$relativePath] = $payloadFile
    }
    $payloadBytes = [int64](($payloadFiles | Measure-Object Length -Sum).Sum)
    if ($payloadFiles.Count -ne [int]$manifest.payload_file_count) {
        $failures.Add("SDK payload file count mismatch under: $Root")
    }
    if ($payloadBytes -ne [int64]$manifest.payload_bytes) {
        $failures.Add("SDK payload byte count mismatch under: $Root")
    }

    $rootPrefix = $rootFullPath + '\'
    $seenPaths = @{}
    foreach ($line in Get-Content -LiteralPath $checksumsPath -Encoding UTF8) {
        if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') {
            $failures.Add("Malformed SDK checksum line under $Root`: $line")
            continue
        }
        $expectedHash = $Matches[1].ToLowerInvariant()
        $relativePath = $Matches[2].Replace('\', '/')
        if ($seenPaths.ContainsKey($relativePath)) {
            $failures.Add("Duplicate SDK checksum path under $Root`: $relativePath")
            continue
        }
        $seenPaths[$relativePath] = $true
        $payloadPath = [System.IO.Path]::GetFullPath((Join-Path $Root $relativePath))
        if (-not $payloadPath.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $failures.Add("SDK checksum path escapes its root: $relativePath")
            continue
        }
        if (-not (Test-Path -LiteralPath $payloadPath -PathType Leaf)) {
            $failures.Add("SDK checksum references a missing file: $payloadPath")
            continue
        }
        if (-not $payloadByPath.ContainsKey($relativePath)) {
            $failures.Add("SDK checksum references an unexpected payload path: $payloadPath")
            continue
        }
        $payloadRepoPath = "$rootRelative/$relativePath"
        if (-not $SkipSdkGitTracking -and -not $trackedSet.Contains($payloadRepoPath)) {
            $failures.Add("SDK payload is not tracked by Git: $payloadRepoPath")
        }
        if ($payloadByPath[$relativePath].Length -ge 10MB) {
            $filterAttribute = if ($SkipSdkGitTracking) {
                @(& git -C $suiteRoot check-attr filter -- $payloadRepoPath)
            }
            else {
                @(& git -C $suiteRoot check-attr --cached filter -- $payloadRepoPath)
            }
            if ($LASTEXITCODE -ne 0 -or $filterAttribute -notmatch ': filter: lfs$') {
                $failures.Add("Large SDK payload is not assigned to Git LFS: $payloadRepoPath")
            }
            elseif (-not $SkipSdkGitTracking) {
                $indexObjectSize = @(& git -C $suiteRoot cat-file -s ":$payloadRepoPath" 2>$null)
                if ($LASTEXITCODE -ne 0 -or $indexObjectSize.Count -ne 1 -or
                    [int64]$indexObjectSize[0] -gt 1024) {
                    $failures.Add("Large SDK payload is not stored as an LFS pointer in the Git index: $payloadRepoPath")
                }
            }
        }
        if ($VerifySdkHashes) {
            $actualHash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actualHash -ne $expectedHash) {
                $failures.Add("SDK payload SHA-256 mismatch: $payloadPath")
            }
        }
    }
    foreach ($relativePath in $payloadByPath.Keys) {
        if (-not $seenPaths.ContainsKey($relativePath)) {
            $failures.Add("SDK payload is missing from SHA256SUMS.txt: $rootRelative/$relativePath")
        }
    }
}

$scanSdkPaths = @(
    'bin/ScanEngineCore.dll', 'bin/scanengine.json',
    'bin/include/cccl', 'bin/include/cuda',
    'include/scanengine/scanengine_api.h', 'include/scanengine/scanengine.hpp',
    'include/scanengine/scanengine_runtime.h', 'include/scanengine/scanengine_version.h',
    'lib/ScanEngineCore.lib', 'lib/ScanEngineRuntime.lib',
    'cmake/ScanEngineConfig.cmake', 'doc/SDK_ABI.md', 'licenses',
    'SDK_MANIFEST.json', 'SHA256SUMS.txt'
)
foreach ($path in $scanSdkPaths) { Require-Path -Root $scanSdk -RelativePath $path }
Require-MatchingFile -Root (Join-Path $scanSdk 'bin/models') -Filter '*.safetensors*'
Require-MatchingFile -Root (Join-Path $scanSdk 'bin/runtimes/cuda') -Filter '*.dll'

$voiceSdkPaths = @(
    'bin/VoiceEngineCore.dll', 'bin/voiceengine.json',
    'include/voiceengine/voiceengine_api.h', 'include/voiceengine/voiceengine.hpp',
    'include/voiceengine/voiceengine_runtime.h', 'include/voiceengine/voiceengine_version.h',
    'lib/VoiceEngineCore.lib', 'lib/VoiceEngineRuntime.lib',
    'cmake/VoiceEngineConfig.cmake', 'doc/SDK_ABI.md', 'licenses',
    'bin/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part1',
    'bin/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part2',
    'bin/models/qwen3-asr-1.7b/mmproj-Qwen3-ASR-1.7B-bf16.gguf',
    'bin/vcomp140.dll',
    'tools/materialize-voice-model.ps1',
    'SDK_MANIFEST.json', 'SHA256SUMS.txt'
)
foreach ($path in $voiceSdkPaths) { Require-Path -Root $voiceSdk -RelativePath $path }
Forbid-Path -Root $voiceSdk -RelativePath 'bin/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf'
Require-MatchingFile -Root (Join-Path $voiceSdk 'bin/runtimes/cuda') -Filter '*.dll'
Require-MatchingFile -Root (Join-Path $voiceSdk 'bin/runtimes/ffmpeg') -Filter '*.dll'

Verify-SdkManifest -Root $scanSdk
Verify-SdkManifest -Root $voiceSdk

if ($VerifyCudaArchitectures) {
    try {
        & (Join-Path (Join-Path $PSScriptRoot 'internal') '验证CUDA架构.ps1') `
            -ScanCorePath (Join-Path $scanSdk 'bin/ScanEngineCore.dll') `
            -VoiceCorePath (Join-Path $voiceSdk 'bin/VoiceEngineCore.dll')
    }
    catch {
        $failures.Add("CUDA architecture verification failed: $($_.Exception.Message)")
    }
}

if ($SdkOnly) {
    if ($failures.Count -gt 0) {
        $failures | ForEach-Object { Write-Error $_ -ErrorAction Continue }
        exit 1
    }
    Write-Output 'SDK layout OK: committed ScanEngine and VoiceEngine baselines.'
    exit 0
}

$studioPaths = @(
    'RecognitionStudio.exe',
    'RecognitionStudioScanWorker.exe',
    'components/scanengine/ScanEngineCore.dll',
    'components/scanengine/SDK_MANIFEST.json',
    'components/scanengine/SDK_SHA256SUMS.txt',
    'components/scanengine/include/cccl',
    'components/scanengine/include/cuda',
    'components/voiceengine/VoiceEngineCore.dll',
    'components/voiceengine/SDK_MANIFEST.json',
    'components/voiceengine/SDK_SHA256SUMS.txt',
    'components/voiceengine/vcomp140.dll',
    'components/voiceengine/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf',
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
    'components/voiceengine/examples',
    'components/voiceengine/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part1',
    'components/voiceengine/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part2'
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

Write-Output 'Artifact layout OK: committed ScanEngine/VoiceEngine SDK baselines and RecognitionStudio runtime.'
