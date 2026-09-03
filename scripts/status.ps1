[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$suiteRoot = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $suiteRoot '.git'))) {
    Write-Output 'RecognitionSuite is not initialized.'
    exit 1
}

& git -C $suiteRoot status --short --branch
Write-Output ''
Write-Output '[components]'
foreach ($component in @('apps/RecognitionStudio', 'engines/ScanEngine', 'engines/VoiceEngine')) {
    $count = @(& git -C $suiteRoot status --short -- $component).Count
    Write-Output ("  {0,-26} {1,4} changed paths" -f $component, $count)
}

Write-Output ''
Write-Output '[published SDKs]'
foreach ($sdk in @(
    @{ Name = 'ScanEngine'; Path = 'sdk/ScanEngine/windows-x64'; Config = 'cmake/ScanEngineConfig.cmake' },
    @{ Name = 'VoiceEngine'; Path = 'sdk/VoiceEngine/windows-x64'; Config = 'cmake/VoiceEngineConfig.cmake' }
)) {
    $sdkRoot = Join-Path $suiteRoot $sdk.Path
    $manifestPath = Join-Path $sdkRoot 'SDK_MANIFEST.json'
    $present = (Test-Path -LiteralPath (Join-Path $sdkRoot $sdk.Config)) -and
        (Test-Path -LiteralPath $manifestPath)
    $state = 'missing'
    if ($present) {
        try {
            $manifest = Get-Content -LiteralPath $manifestPath -Encoding UTF8 -Raw | ConvertFrom-Json
            $trackedCount = @(& git -C $suiteRoot ls-files -- $sdk.Path).Count
            $expectedTrackedCount = [int]$manifest.payload_file_count + 2
            $state = if ($trackedCount -eq $expectedTrackedCount) { 'ready' } else { 'present, not fully tracked' }
        }
        catch {
            $state = 'invalid manifest'
        }
    }
    Write-Output ("  {0,-26} {1}" -f $sdk.Name, $state)
}
