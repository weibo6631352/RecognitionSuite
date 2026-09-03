[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$suiteRoot = Split-Path -Parent $PSScriptRoot
$artifactsRoot = Join-Path $suiteRoot 'artifacts'
$stagingRoot = Join-Path $artifactsRoot 'sdk-publish-staging'
$backupRoot = Join-Path $artifactsRoot 'sdk-publish-backup'
$publishedRoot = Join-Path $suiteRoot 'sdk'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Assert-ChildPath {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Root
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to operate outside $fullRoot`: $fullPath"
    }
    return $fullPath
}

function Remove-VerifiedTree {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$AllowedRoot
    )

    $verifiedPath = Assert-ChildPath -Path $Path -Root $AllowedRoot
    if (Test-Path -LiteralPath $verifiedPath) {
        Remove-Item -LiteralPath $verifiedPath -Recurse -Force
    }
}

function Copy-SdkTree {
    param(
        [Parameter(Mandatory)] [string]$Source,
        [Parameter(Mandatory)] [string]$Destination,
        [string[]]$ExcludeFiles = @('.sdk.stamp')
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Generated SDK is missing: $Source"
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $copyArguments = @(
        $Source, $Destination,
        '/E', '/COPY:DAT', '/DCOPY:DAT', '/R:2', '/W:1',
        '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/XJ', '/XF'
    ) + $ExcludeFiles
    & robocopy @copyArguments
    $copyExitCode = $LASTEXITCODE
    if ($copyExitCode -ge 8) {
        throw "robocopy failed with exit code $copyExitCode while publishing $Source"
    }
}

function Write-SdkManifest {
    param(
        [Parameter(Mandatory)] [string]$SdkRoot,
        [Parameter(Mandatory)] [string]$Engine,
        [Parameter(Mandatory)] [string]$BuildPreset,
        [Parameter(Mandatory)] [string]$SourceCommit,
        [Parameter(Mandatory)] [string]$SourceTree,
        [Parameter(Mandatory)] [string]$ModelPackaging
    )

    $checksumsPath = Join-Path $SdkRoot 'SHA256SUMS.txt'
    $manifestPath = Join-Path $SdkRoot 'SDK_MANIFEST.json'
    $payloadFiles = @(
        Get-ChildItem -LiteralPath $SdkRoot -File -Recurse -Force |
            Where-Object { $_.FullName -notin @($checksumsPath, $manifestPath) } |
            Sort-Object FullName
    )
    $checksumLines = [System.Collections.Generic.List[string]]::new()
    $payloadBytes = [int64]0
    foreach ($file in $payloadFiles) {
        $relativePath = $file.FullName.Substring($SdkRoot.Length + 1).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $checksumLines.Add("$hash  $relativePath")
        $payloadBytes += $file.Length
    }
    [System.IO.File]::WriteAllLines($checksumsPath, $checksumLines, $utf8NoBom)
    $checksumsHash = (Get-FileHash -LiteralPath $checksumsPath -Algorithm SHA256).Hash.ToLowerInvariant()

    $manifest = [ordered]@{
        schema_version = 1
        engine = $Engine
        engine_version = '0.1.0'
        sdk_api_version = 1
        platform = 'windows-x64'
        source_commit = $SourceCommit
        source_tree = $SourceTree
        build_preset = $BuildPreset
        payload_format = 'expanded-directory'
        model_packaging = $ModelPackaging
        payload_file_count = $payloadFiles.Count
        payload_bytes = $payloadBytes
        checksums_file = 'SHA256SUMS.txt'
        checksums_sha256 = $checksumsHash
        generated_by = 'scripts/publish-sdks.ps1'
    }
    $json = $manifest | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText($manifestPath, $json + "`n", $utf8NoBom)
}

function Assert-SdkLayout {
    param(
        [Parameter(Mandatory)] [string]$SdkRoot,
        [Parameter(Mandatory)] [string]$Engine
    )

    $engineLower = $Engine.ToLowerInvariant()
    $requiredPaths = @(
        "bin/$($Engine)Core.dll",
        "bin/$engineLower.json",
        "include/$engineLower/$($engineLower)_api.h",
        "include/$engineLower/$($engineLower)_runtime.h",
        "lib/$($Engine)Core.lib",
        "lib/$($Engine)Runtime.lib",
        "cmake/$($Engine)Config.cmake",
        'doc/SDK_ABI.md',
        'examples',
        'licenses',
        'SDK_MANIFEST.json',
        'SHA256SUMS.txt'
    )
    if ($Engine -eq 'ScanEngine') {
        $requiredPaths += @('bin/models', 'bin/runtimes/cuda')
    }
    else {
        $requiredPaths += @(
            'bin/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part1',
            'bin/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf.part2',
            'bin/models/qwen3-asr-1.7b/mmproj-Qwen3-ASR-1.7B-bf16.gguf',
            'bin/runtimes/cuda',
            'bin/runtimes/ffmpeg',
            'tools/materialize-voice-model.ps1'
        )
    }

    foreach ($relativePath in $requiredPaths) {
        if (-not (Test-Path -LiteralPath (Join-Path $SdkRoot $relativePath))) {
            throw "Published SDK is incomplete; missing $relativePath under $SdkRoot"
        }
    }

    if (-not (Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'licenses') -File -Recurse |
            Select-Object -First 1)) {
        throw "Published SDK has no license files under $SdkRoot"
    }
    if (-not (Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'bin/runtimes/cuda') -Filter '*.dll' -File |
            Select-Object -First 1)) {
        throw "Published SDK has no CUDA runtime DLLs under $SdkRoot"
    }
    if ($Engine -eq 'ScanEngine') {
        if (-not (Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'bin/models') -Filter '*.safetensors*' -File -Recurse |
                Select-Object -First 1)) {
            throw "Published ScanEngine SDK has no model weights under $SdkRoot"
        }
    }
    else {
        if (-not (Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'bin/runtimes/ffmpeg') -Filter '*.dll' -File |
                Select-Object -First 1)) {
            throw "Published VoiceEngine SDK has no FFmpeg runtime DLLs under $SdkRoot"
        }
        $fullModel = Join-Path $SdkRoot 'bin/models/qwen3-asr-1.7b/Qwen3-ASR-1.7B-bf16.gguf'
        if (Test-Path -LiteralPath $fullModel) {
            throw "Published VoiceEngine SDK must contain model parts, not the oversized full GGUF: $fullModel"
        }
    }

    if (Get-ChildItem -LiteralPath $SdkRoot -Filter '.sdk.stamp' -File -Recurse -Force |
            Select-Object -First 1) {
        throw "Published SDK contains an engine-local build stamp under $SdkRoot"
    }
    $oversizedFile = Get-ChildItem -LiteralPath $SdkRoot -File -Recurse -Force |
        Where-Object { $_.Length -ge 2GB } |
        Select-Object -First 1
    if ($oversizedFile) {
        throw "Published SDK contains a file at or above the 2 GiB remote limit: $($oversizedFile.FullName)"
    }
}

function Assert-SdkManifest {
    param(
        [Parameter(Mandatory)] [string]$SdkRoot,
        [switch]$VerifyHashes
    )

    $manifestPath = Join-Path $SdkRoot 'SDK_MANIFEST.json'
    $checksumsPath = Join-Path $SdkRoot 'SHA256SUMS.txt'
    $manifest = Get-Content -LiteralPath $manifestPath -Encoding UTF8 -Raw | ConvertFrom-Json
    $actualChecksumsHash = (Get-FileHash -LiteralPath $checksumsPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualChecksumsHash -ne $manifest.checksums_sha256) {
        throw "SDK checksum manifest hash mismatch: $checksumsPath"
    }

    $payloadFiles = @(
        Get-ChildItem -LiteralPath $SdkRoot -File -Recurse -Force |
            Where-Object { $_.FullName -notin @($manifestPath, $checksumsPath) }
    )
    $payloadByPath = @{}
    $payloadBytes = [int64]0
    foreach ($file in $payloadFiles) {
        $relativePath = $file.FullName.Substring($SdkRoot.Length + 1).Replace('\', '/')
        $payloadByPath[$relativePath] = $file
        $payloadBytes += $file.Length
    }
    if ($payloadFiles.Count -ne [int]$manifest.payload_file_count -or
        $payloadBytes -ne [int64]$manifest.payload_bytes) {
        throw "SDK payload count or byte total does not match $manifestPath"
    }

    $seenPaths = @{}
    foreach ($line in Get-Content -LiteralPath $checksumsPath -Encoding UTF8) {
        if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') {
            throw "Malformed SDK checksum line under $SdkRoot`: $line"
        }
        $expectedHash = $Matches[1].ToLowerInvariant()
        $relativePath = $Matches[2]
        if ($seenPaths.ContainsKey($relativePath)) {
            throw "Duplicate SDK checksum path under $SdkRoot`: $relativePath"
        }
        if (-not $payloadByPath.ContainsKey($relativePath)) {
            throw "SDK checksum references a missing or unexpected path under $SdkRoot`: $relativePath"
        }
        $seenPaths[$relativePath] = $true
        if ($VerifyHashes) {
            $actualHash = (Get-FileHash -LiteralPath $payloadByPath[$relativePath].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actualHash -ne $expectedHash) {
                throw "SDK payload SHA-256 mismatch: $relativePath under $SdkRoot"
            }
        }
    }
    if ($seenPaths.Count -ne $payloadByPath.Count) {
        $missingPaths = @($payloadByPath.Keys | Where-Object { -not $seenPaths.ContainsKey($_) })
        throw "SDK checksum manifest does not cover every payload under $SdkRoot`: $($missingPaths -join ', ')"
    }
}

$enginePaths = @('engines/ScanEngine', 'engines/VoiceEngine')
& git -C $suiteRoot diff --quiet -- $enginePaths
$unstagedExitCode = $LASTEXITCODE
if ($unstagedExitCode -eq 1) {
    $unstagedChanges = @(& git -C $suiteRoot diff --name-only -- $enginePaths)
    throw "Stage or restore engine source changes before publishing SDKs:`n$($unstagedChanges -join "`n")"
}
if ($unstagedExitCode -ne 0) {
    throw 'Unable to inspect unstaged engine source changes.'
}
$untrackedEngineFiles = @(& git -C $suiteRoot ls-files --others --exclude-standard -- $enginePaths)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to inspect untracked engine source files.'
}
if ($untrackedEngineFiles.Count -gt 0) {
    throw "Stage or remove untracked engine source files before publishing SDKs:`n$($untrackedEngineFiles -join "`n")"
}
$sourceCommit = (& git -C $suiteRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to resolve the source commit.'
}
$indexTree = (& git -C $suiteRoot write-tree).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to resolve the staged source tree.'
}

$sdkSpecs = @(
    [ordered]@{
        Name = 'ScanEngine'
        RepositoryPath = 'engines/ScanEngine'
        SourceTree = $null
        Source = Join-Path $suiteRoot 'engines/ScanEngine/build/win-qt5.12.9-msvc-mlx-cuda/sdk'
        Preset = 'win-qt5.12.9-msvc-mlx-cuda'
        ModelPackaging = 'native-safetensors-parts'
    },
    [ordered]@{
        Name = 'VoiceEngine'
        RepositoryPath = 'engines/VoiceEngine'
        SourceTree = $null
        Source = Join-Path $suiteRoot 'engines/VoiceEngine/build/win-qt5.12.9-msvc-cuda/sdk'
        Preset = 'win-qt5.12.9-msvc-cuda'
        ModelPackaging = 'gguf-part1-part2; materialize before distribution'
    }
)

foreach ($spec in $sdkSpecs) {
    $spec.SourceTree = (& git -C $suiteRoot rev-parse "$indexTree`:$($spec.RepositoryPath)").Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to resolve staged source tree for $($spec.RepositoryPath)."
    }
}

$publishedSdkPaths = @('sdk/ScanEngine/windows-x64', 'sdk/VoiceEngine/windows-x64')
$trackedPublishedFiles = @(& git -C $suiteRoot ls-files -- $publishedSdkPaths)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to inspect the published SDK baseline.'
}
if ($trackedPublishedFiles.Count -gt 0) {
    $publishedChanges = @(& git -C $suiteRoot status --porcelain --untracked-files=all -- $publishedSdkPaths)
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to inspect changes in the published SDK baseline.'
    }
    if ($publishedChanges.Count -gt 0) {
        $changePreview = @($publishedChanges | Select-Object -First 20)
        if ($publishedChanges.Count -gt $changePreview.Count) {
            $changePreview += "... and $($publishedChanges.Count - $changePreview.Count) more paths"
        }
        throw "Commit or restore existing root SDK changes before refreshing the baseline:`n$($changePreview -join "`n")"
    }
}

New-Item -ItemType Directory -Path $artifactsRoot -Force | Out-Null
Remove-VerifiedTree -Path $stagingRoot -AllowedRoot $artifactsRoot
if (Test-Path -LiteralPath $backupRoot) {
    $backupEntries = @(Get-ChildItem -LiteralPath $backupRoot -Force)
    if ($backupEntries.Count -gt 0) {
        throw "A previous SDK recovery backup still exists at $backupRoot. Inspect and restore it before publishing again."
    }
    Remove-VerifiedTree -Path $backupRoot -AllowedRoot $artifactsRoot
}
New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null

foreach ($spec in $sdkSpecs) {
    $stageSdk = Join-Path $stagingRoot $spec.Name
    $excludes = @('.sdk.stamp')
    if ($spec.Name -eq 'VoiceEngine') {
        $excludes += 'Qwen3-ASR-1.7B-bf16.gguf'
    }
    Copy-SdkTree -Source $spec.Source -Destination $stageSdk -ExcludeFiles $excludes

    if ($spec.Name -eq 'VoiceEngine') {
        $modelSource = Join-Path $suiteRoot 'engines/VoiceEngine/models/qwen3-asr-1.7b'
        $modelDestination = Join-Path $stageSdk 'bin/models/qwen3-asr-1.7b'
        foreach ($partName in @(
            'Qwen3-ASR-1.7B-bf16.gguf.part1',
            'Qwen3-ASR-1.7B-bf16.gguf.part2'
        )) {
            Copy-Item -LiteralPath (Join-Path $modelSource $partName) -Destination $modelDestination -Force
        }
        $toolsDestination = Join-Path $stageSdk 'tools'
        New-Item -ItemType Directory -Path $toolsDestination -Force | Out-Null
        $materializerSource = Join-Path $PSScriptRoot 'materialize-voice-model.ps1'
        $materializerDestination = Join-Path $toolsDestination 'materialize-voice-model.ps1'
        $materializerText = [System.IO.File]::ReadAllText($materializerSource)
        $materializerText = $materializerText.Replace("`r`n", "`n").Replace("`r", "`n")
        [System.IO.File]::WriteAllText($materializerDestination, $materializerText, $utf8NoBom)
    }

    Write-SdkManifest `
        -SdkRoot $stageSdk `
        -Engine $spec.Name `
        -BuildPreset $spec.Preset `
        -SourceCommit $sourceCommit `
        -SourceTree $spec.SourceTree `
        -ModelPackaging $spec.ModelPackaging
    Assert-SdkLayout -SdkRoot $stageSdk -Engine $spec.Name
    Assert-SdkManifest -SdkRoot $stageSdk -VerifyHashes
}

New-Item -ItemType Directory -Path $publishedRoot -Force | Out-Null
New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
$installed = [System.Collections.Generic.List[string]]::new()
$backedUp = [System.Collections.Generic.List[object]]::new()
try {
    foreach ($spec in $sdkSpecs) {
        $engineRoot = Join-Path $publishedRoot $spec.Name
        $destination = Join-Path $engineRoot 'windows-x64'
        New-Item -ItemType Directory -Path $engineRoot -Force | Out-Null
        if (Test-Path -LiteralPath $destination) {
            $backupEngineRoot = Join-Path $backupRoot $spec.Name
            New-Item -ItemType Directory -Path $backupEngineRoot -Force | Out-Null
            $backupDestination = Join-Path $backupEngineRoot 'windows-x64'
            Move-Item -LiteralPath $destination -Destination $backupDestination
            $backedUp.Add([PSCustomObject]@{ Source = $backupDestination; Destination = $destination })
        }
    }

    foreach ($spec in $sdkSpecs) {
        $stageSdk = Join-Path $stagingRoot $spec.Name
        $destination = Join-Path (Join-Path $publishedRoot $spec.Name) 'windows-x64'
        Move-Item -LiteralPath $stageSdk -Destination $destination
        $installed.Add($destination)
    }

    foreach ($spec in $sdkSpecs) {
        $destination = Join-Path (Join-Path $publishedRoot $spec.Name) 'windows-x64'
        Assert-SdkLayout -SdkRoot $destination -Engine $spec.Name
        Assert-SdkManifest -SdkRoot $destination
    }
}
catch {
    foreach ($destination in $installed) {
        Remove-VerifiedTree -Path $destination -AllowedRoot $publishedRoot
    }
    foreach ($backup in $backedUp) {
        if (Test-Path -LiteralPath $backup.Source) {
            Move-Item -LiteralPath $backup.Source -Destination $backup.Destination
        }
    }
    throw
}

Remove-VerifiedTree -Path $stagingRoot -AllowedRoot $artifactsRoot
Remove-VerifiedTree -Path $backupRoot -AllowedRoot $artifactsRoot
Write-Output "Published validated SDK baselines from $sourceCommit under $publishedRoot"
