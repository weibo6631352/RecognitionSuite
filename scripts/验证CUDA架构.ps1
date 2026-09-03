[CmdletBinding()]
param(
    [string]$ScanCorePath,
    [string]$VoiceCorePath,
    [string]$CuobjdumpPath
)

$ErrorActionPreference = 'Stop'
$suiteRoot = Split-Path -Parent $PSScriptRoot

if (-not $ScanCorePath) {
    $ScanCorePath = Join-Path $suiteRoot 'sdk/ScanEngine/windows-x64/bin/ScanEngineCore.dll'
}
if (-not $VoiceCorePath) {
    $VoiceCorePath = Join-Path $suiteRoot 'sdk/VoiceEngine/windows-x64/bin/VoiceEngineCore.dll'
}

function Resolve-Cuobjdump {
    if ($CuobjdumpPath) {
        if (-not (Test-Path -LiteralPath $CuobjdumpPath -PathType Leaf)) {
            throw "cuobjdump was not found: $CuobjdumpPath"
        }
        return (Resolve-Path -LiteralPath $CuobjdumpPath).Path
    }

    $command = Get-Command cuobjdump.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($env:CUDA_PATH) {
        $candidates.Add((Join-Path $env:CUDA_PATH 'bin/cuobjdump.exe'))
    }
    $candidates.Add('C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin\cuobjdump.exe')
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw 'CUDA cuobjdump.exe is required for the release architecture check. Install CUDA Toolkit 12.9 or pass -CuobjdumpPath.'
}

function Get-CudaEntries {
    param(
        [Parameter(Mandatory)] [string]$Tool,
        [Parameter(Mandatory)] [string]$CorePath,
        [Parameter(Mandatory)] [ValidateSet('elf', 'ptx')] [string]$Kind
    )

    if (-not (Test-Path -LiteralPath $CorePath -PathType Leaf)) {
        throw "Engine core DLL was not found: $CorePath"
    }
    $option = if ($Kind -eq 'elf') { '--list-elf' } else { '--list-ptx' }
    $output = @(& $Tool $option $CorePath 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "cuobjdump $option failed for $CorePath`n$($output -join "`n")"
    }

    $entries = @(
        $output | ForEach-Object {
            if ([string]$_ -match '\.(sm_[0-9]+a?)\.(?:cubin|ptx)\s*$') {
                $Matches[1]
            }
        }
    )
    if ($entries.Count -eq 0) {
        throw "No CUDA $Kind entries were found in $CorePath"
    }
    return $entries
}

function Assert-ContainsArchitecture {
    param(
        [Parameter(Mandatory)] [string[]]$Architectures,
        [Parameter(Mandatory)] [string]$Required,
        [Parameter(Mandatory)] [string]$Description
    )

    if ($Required -notin $Architectures) {
        $actual = @($Architectures | Sort-Object -Unique) -join ', '
        throw "$Description is missing required architecture $Required; found: $actual"
    }
}

function Assert-CoreArchitectures {
    param(
        [Parameter(Mandatory)] [string]$Tool,
        [Parameter(Mandatory)] [string]$Engine,
        [Parameter(Mandatory)] [string]$CorePath,
        [Parameter(Mandatory)] [string[]]$RequiredPtxBlackwell
    )

    $nativeEntries = @(Get-CudaEntries -Tool $Tool -CorePath $CorePath -Kind elf)
    $ptxEntries = @(Get-CudaEntries -Tool $Tool -CorePath $CorePath -Kind ptx)
    Assert-ContainsArchitecture -Architectures $nativeEntries -Required 'sm_89' `
        -Description "$Engine native CUDA image"
    Assert-ContainsArchitecture -Architectures $nativeEntries -Required 'sm_120a' `
        -Description "$Engine native CUDA image"
    Assert-ContainsArchitecture -Architectures $ptxEntries -Required 'sm_89' `
        -Description "$Engine PTX image"

    $hasBlackwellPtx = @($RequiredPtxBlackwell | Where-Object { $_ -in $ptxEntries }).Count -gt 0
    if (-not $hasBlackwellPtx) {
        $actual = @($ptxEntries | Sort-Object -Unique) -join ', '
        throw "$Engine PTX image is missing an sm_120/sm_120a Blackwell target; found: $actual"
    }

    $nativeSummary = @(
        $nativeEntries | Group-Object | Sort-Object Name |
            ForEach-Object { "$($_.Name)=$($_.Count)" }
    ) -join ', '
    $ptxSummary = @(
        $ptxEntries | Group-Object | Sort-Object Name |
            ForEach-Object { "$($_.Name)=$($_.Count)" }
    ) -join ', '
    Write-Output "$Engine CUDA architectures OK: native [$nativeSummary]; PTX [$ptxSummary]."
}

$cuobjdump = Resolve-Cuobjdump
Assert-CoreArchitectures -Tool $cuobjdump -Engine 'ScanEngine' `
    -CorePath $ScanCorePath -RequiredPtxBlackwell @('sm_120', 'sm_120a')
Assert-CoreArchitectures -Tool $cuobjdump -Engine 'VoiceEngine' `
    -CorePath $VoiceCorePath -RequiredPtxBlackwell @('sm_120a', 'sm_120')
