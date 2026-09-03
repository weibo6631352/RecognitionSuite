[CmdletBinding()]
param(
    [ValidateSet('Studio', 'All', 'SDKs', 'Engines', 'ScanEngine', 'VoiceEngine')]
    [string]$Target = 'Studio',
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$suiteRoot = Split-Path -Parent $PSScriptRoot

function Invoke-CMakePreset {
    param(
        [Parameter(Mandatory)] [string]$Repository,
        [Parameter(Mandatory)] [string]$Preset,
        [string]$BuildTarget
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
            $buildArguments = @('--build', '--preset', $Preset)
            if ($BuildTarget) {
                $buildArguments += @('--target', $BuildTarget)
            }
            & cmake @buildArguments
            if ($LASTEXITCODE -ne 0) {
                throw "CMake build failed for $Repository"
            }
        }
    }
    finally {
        Pop-Location
    }
}

function Assert-EngineSourcesReadyForPublish {
    $enginePaths = @('engines/ScanEngine', 'engines/VoiceEngine')
    & git -C $suiteRoot diff --quiet -- $enginePaths
    $unstagedExitCode = $LASTEXITCODE
    if ($unstagedExitCode -eq 1) {
        $unstagedChanges = @(& git -C $suiteRoot diff --name-only -- $enginePaths)
        throw "Stage or restore engine source changes before building publishable SDKs:`n$($unstagedChanges -join "`n")"
    }
    if ($unstagedExitCode -ne 0) {
        throw 'Unable to inspect unstaged engine source changes.'
    }
    $untrackedEngineFiles = @(& git -C $suiteRoot ls-files --others --exclude-standard -- $enginePaths)
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to inspect untracked engine source files.'
    }
    if ($untrackedEngineFiles.Count -gt 0) {
        throw "Stage or remove untracked engine source files before building publishable SDKs:`n$($untrackedEngineFiles -join "`n")"
    }
}

function Assert-PublishedSdkBaselineReadyForRefresh {
    $sdkPaths = @('sdk/ScanEngine/windows-x64', 'sdk/VoiceEngine/windows-x64')
    $trackedSdkFiles = @(& git -C $suiteRoot ls-files -- $sdkPaths)
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to inspect the published SDK baseline.'
    }
    if ($trackedSdkFiles.Count -eq 0) {
        return
    }
    $sdkChanges = @(& git -C $suiteRoot status --porcelain --untracked-files=all -- $sdkPaths)
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to inspect changes in the published SDK baseline.'
    }
    if ($sdkChanges.Count -gt 0) {
        $changePreview = @($sdkChanges | Select-Object -First 20)
        if ($sdkChanges.Count -gt $changePreview.Count) {
            $changePreview += "... and $($sdkChanges.Count - $changePreview.Count) more paths"
        }
        throw "Commit or restore existing root SDK changes before rebuilding publishable SDKs:`n$($changePreview -join "`n")"
    }
}

if ($Target -in @('All', 'SDKs')) {
    Assert-EngineSourcesReadyForPublish
    if (-not $ConfigureOnly) {
        Assert-PublishedSdkBaselineReadyForRefresh
    }
}

if ($Target -in @('All', 'SDKs', 'Engines', 'ScanEngine')) {
    $buildTarget = if ($Target -in @('All', 'SDKs')) { 'ScanEngineSDK' } else { $null }
    Invoke-CMakePreset -Repository 'engines/ScanEngine' `
        -Preset 'win-qt5.12.9-msvc-mlx-cuda' -BuildTarget $buildTarget
}

if ($Target -in @('All', 'SDKs', 'Engines', 'VoiceEngine')) {
    $buildTarget = if ($Target -in @('All', 'SDKs')) { 'sdk-bin' } else { $null }
    Invoke-CMakePreset -Repository 'engines/VoiceEngine' `
        -Preset 'win-qt5.12.9-msvc-cuda' -BuildTarget $buildTarget
}

if (-not $ConfigureOnly -and $Target -in @('All', 'SDKs')) {
    & (Join-Path $PSScriptRoot '发布SDK.ps1')
}
elseif (-not $ConfigureOnly -and $Target -in @('Engines', 'ScanEngine', 'VoiceEngine')) {
    Write-Output 'Engine staging build complete; the committed root SDK baseline was not changed. Use -Target SDKs to refresh it.'
}

if ($Target -in @('All', 'Studio')) {
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot '验证成果.ps1') `
        -SdkOnly -SkipSdkGitTracking
    if ($LASTEXITCODE -ne 0) {
        throw 'Published SDK baseline verification failed.'
    }
    Invoke-CMakePreset -Repository 'apps/RecognitionStudio' -Preset 'windows-msvc-qt5'
}
