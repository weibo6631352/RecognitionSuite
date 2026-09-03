[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$ModelDirectory,

    [switch]$RemoveParts
)

$ErrorActionPreference = 'Stop'

$modelRoot = [System.IO.Path]::GetFullPath($ModelDirectory)
if (-not [System.IO.Directory]::Exists($modelRoot)) {
    throw "VoiceEngine model directory does not exist: $modelRoot"
}

$checksumsPath = Join-Path $modelRoot 'SHA256SUMS.txt'
if (-not (Test-Path -LiteralPath $checksumsPath -PathType Leaf)) {
    throw "VoiceEngine model checksum manifest is missing: $checksumsPath"
}

$expectedHashes = @{}
foreach ($line in Get-Content -LiteralPath $checksumsPath -Encoding UTF8) {
    if ($line -match '^([0-9a-fA-F]{64})\s+(.+)$') {
        $expectedHashes[$Matches[2].Trim()] = $Matches[1].ToLowerInvariant()
    }
}

$fullName = 'Qwen3-ASR-1.7B-bf16.gguf'
$partNames = @("$fullName.part1", "$fullName.part2")
$projectionName = 'mmproj-Qwen3-ASR-1.7B-bf16.gguf'
foreach ($requiredName in @($fullName, $projectionName)) {
    if (-not $expectedHashes.ContainsKey($requiredName)) {
        throw "Missing checksum for $requiredName in $checksumsPath"
    }
}

function Assert-FileHash {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$ExpectedHash
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required VoiceEngine model file is missing: $Path"
    }
    $actualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $ExpectedHash) {
        throw "SHA-256 mismatch for $Path; expected $ExpectedHash, got $actualHash"
    }
}

$projectionPath = Join-Path $modelRoot $projectionName
Assert-FileHash -Path $projectionPath -ExpectedHash $expectedHashes[$projectionName]

$fullPath = Join-Path $modelRoot $fullName
if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
    Assert-FileHash -Path $fullPath -ExpectedHash $expectedHashes[$fullName]
}
else {
    foreach ($partName in $partNames) {
        if (-not $expectedHashes.ContainsKey($partName)) {
            throw "Missing checksum for $partName in $checksumsPath"
        }
    }
    $partPaths = @($partNames | ForEach-Object { Join-Path $modelRoot $_ })
    $expectedLength = [int64]0
    foreach ($partPath in $partPaths) {
        if (-not (Test-Path -LiteralPath $partPath -PathType Leaf)) {
            throw "Required VoiceEngine model part is missing: $partPath"
        }
        $expectedLength += (Get-Item -LiteralPath $partPath).Length
    }

    $temporaryPath = "$fullPath.assembling-$PID.tmp"
    if (Test-Path -LiteralPath $temporaryPath) {
        Remove-Item -LiteralPath $temporaryPath -Force
    }

    $sha256 = $null
    $destinationStream = $null
    $hashingStream = $null
    try {
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        $destinationStream = [System.IO.File]::Open(
            $temporaryPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None)
        $hashingStream = [System.Security.Cryptography.CryptoStream]::new(
            $destinationStream,
            $sha256,
            [System.Security.Cryptography.CryptoStreamMode]::Write)

        foreach ($partPath in $partPaths) {
            $sourceStream = [System.IO.File]::OpenRead($partPath)
            try {
                $sourceStream.CopyTo($hashingStream, 8MB)
            }
            finally {
                $sourceStream.Dispose()
            }
        }

        $hashingStream.FlushFinalBlock()
        $actualHash = [System.BitConverter]::ToString($sha256.Hash).Replace('-', '').ToLowerInvariant()
        $hashingStream.Dispose()
        $hashingStream = $null
        $destinationStream = $null
        $sha256.Dispose()
        $sha256 = $null

        $actualLength = (Get-Item -LiteralPath $temporaryPath).Length
        if ($actualLength -ne $expectedLength) {
            throw "Assembled VoiceEngine model has the wrong size; expected $expectedLength, got $actualLength"
        }
        if ($actualHash -ne $expectedHashes[$fullName]) {
            throw "Assembled VoiceEngine model SHA-256 mismatch; expected $($expectedHashes[$fullName]), got $actualHash"
        }

        [System.IO.File]::Move($temporaryPath, $fullPath)
    }
    finally {
        if ($hashingStream) { $hashingStream.Dispose() }
        elseif ($destinationStream) { $destinationStream.Dispose() }
        if ($sha256) { $sha256.Dispose() }
        if (Test-Path -LiteralPath $temporaryPath) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

if ($RemoveParts) {
    $partPaths = @($partNames | ForEach-Object { Join-Path $modelRoot $_ })
    foreach ($partPath in $partPaths) {
        if (Test-Path -LiteralPath $partPath -PathType Leaf) {
            Remove-Item -LiteralPath $partPath -Force
        }
    }
}

Write-Output "VoiceEngine model ready: $fullPath"
