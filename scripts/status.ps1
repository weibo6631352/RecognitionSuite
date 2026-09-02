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
