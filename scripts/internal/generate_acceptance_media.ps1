$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Speech

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$datasetRoot = Join-Path $repositoryRoot 'testdata\acceptance\codex-independent-2026-09'
$scanRoot = Join-Path $datasetRoot 'scan'
$voiceRoot = Join-Path $datasetRoot 'voice'

[System.IO.Directory]::CreateDirectory((Join-Path $scanRoot 'input')) | Out-Null
[System.IO.Directory]::CreateDirectory((Join-Path $voiceRoot 'input')) | Out-Null

function Read-Truth([string] $path) {
    return [System.IO.File]::ReadAllText($path, [System.Text.UTF8Encoding]::new($false)).Trim()
}

function New-ScanPage {
    param(
        [string] $Id,
        [string] $FontName,
        [float] $BodySize,
        [System.Drawing.Color] $Foreground,
        [System.Drawing.Color] $Background,
        [bool] $RotateClockwise,
        [int] $NoiseSeed
    )

    $truthPath = Join-Path $scanRoot "truth\$Id.txt"
    $outputPath = Join-Path $scanRoot "input\$Id.png"
    $lines = (Read-Truth $truthPath) -split "`r?`n"
    $bitmap = [System.Drawing.Bitmap]::new(1600, 1200,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear($Background)
        $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $bodyFont = [System.Drawing.Font]::new($FontName, $BodySize,
            [System.Drawing.FontStyle]::Regular,
            [System.Drawing.GraphicsUnit]::Pixel)
        $titleFont = [System.Drawing.Font]::new($FontName, $BodySize + 14,
            [System.Drawing.FontStyle]::Bold,
            [System.Drawing.GraphicsUnit]::Pixel)
        $brush = [System.Drawing.SolidBrush]::new($Foreground)
        try {
            $y = 105.0
            for ($index = 0; $index -lt $lines.Count; $index++) {
                $font = if ($index -eq 0) { $titleFont } else { $bodyFont }
                $graphics.DrawString($lines[$index], $font, $brush, 120.0, $y)
                $y += if ($index -eq 0) { 105.0 } else { 82.0 }
            }
        }
        finally {
            $brush.Dispose()
            $titleFont.Dispose()
            $bodyFont.Dispose()
        }

        if ($NoiseSeed -ge 0) {
            $random = [System.Random]::new($NoiseSeed)
            for ($index = 0; $index -lt 2200; $index++) {
                $x = $random.Next(0, $bitmap.Width)
                $y = $random.Next(0, $bitmap.Height)
                $shade = $random.Next(205, 239)
                $bitmap.SetPixel($x, $y,
                    [System.Drawing.Color]::FromArgb($shade, $shade, $shade))
            }
        }
    }
    finally {
        $graphics.Dispose()
    }

    try {
        if ($RotateClockwise) {
            $bitmap.RotateFlip([System.Drawing.RotateFlipType]::Rotate90FlipNone)
        }
        $bitmap.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $bitmap.Dispose()
    }
}

$scanCases = @(
    @{ Id = 'scan-001'; Font = 'Microsoft YaHei'; Size = 43; Fore = '#202020'; Back = '#FFFFFF'; Rotate = $false; Noise = -1 },
    @{ Id = 'scan-002'; Font = 'SimSun'; Size = 45; Fore = '#101010'; Back = '#FAFAF7'; Rotate = $false; Noise = -1 },
    @{ Id = 'scan-003'; Font = 'Microsoft YaHei'; Size = 39; Fore = '#262626'; Back = '#FFFFFF'; Rotate = $false; Noise = -1 },
    @{ Id = 'scan-004'; Font = 'SimSun'; Size = 40; Fore = '#303030'; Back = '#F8F5ED'; Rotate = $false; Noise = 20260903 },
    @{ Id = 'scan-005'; Font = 'Microsoft YaHei'; Size = 42; Fore = '#555555'; Back = '#EEEEEA'; Rotate = $false; Noise = 980001 },
    @{ Id = 'scan-006'; Font = 'SimSun'; Size = 41; Fore = '#181818'; Back = '#FFFFFF'; Rotate = $true; Noise = -1 },
    @{ Id = 'scan-007'; Font = 'Microsoft YaHei'; Size = 42; Fore = '#242424'; Back = '#FFFFFF'; Rotate = $false; Noise = 20260007 },
    @{ Id = 'scan-008'; Font = 'SimSun'; Size = 38; Fore = '#333333'; Back = '#F5F5F5'; Rotate = $false; Noise = -1 }
)

foreach ($case in $scanCases) {
    New-ScanPage -Id $case.Id -FontName $case.Font -BodySize $case.Size `
        -Foreground ([System.Drawing.ColorTranslator]::FromHtml($case.Fore)) `
        -Background ([System.Drawing.ColorTranslator]::FromHtml($case.Back)) `
        -RotateClockwise $case.Rotate -NoiseSeed $case.Noise
}

$voiceCases = @(
    @{ Id = 'voice-001'; Voice = 'Microsoft Huihui Desktop'; Rate = -1 },
    @{ Id = 'voice-002'; Voice = 'Microsoft Huihui Desktop'; Rate = 0 },
    @{ Id = 'voice-003'; Voice = 'Microsoft Huihui Desktop'; Rate = 1 },
    @{ Id = 'voice-004'; Voice = 'Microsoft Huihui Desktop'; Rate = -2 },
    @{ Id = 'voice-005'; Voice = 'Microsoft Huihui Desktop'; Rate = 2 },
    @{ Id = 'voice-006'; Voice = 'Microsoft Huihui Desktop'; Rate = 0 },
    @{ Id = 'voice-007'; Voice = 'Microsoft Zira Desktop'; Rate = 0 },
    @{ Id = 'voice-008'; Voice = 'Microsoft Huihui Desktop'; Rate = 1 }
)

foreach ($case in $voiceCases) {
    $truthPath = Join-Path $voiceRoot "truth\$($case.Id).txt"
    $outputPath = Join-Path $voiceRoot "input\$($case.Id).wav"
    $synthesizer = [System.Speech.Synthesis.SpeechSynthesizer]::new()
    try {
        $synthesizer.SelectVoice($case.Voice)
        $synthesizer.Rate = $case.Rate
        $synthesizer.Volume = 92
        $synthesizer.SetOutputToWaveFile($outputPath)
        $synthesizer.Speak((Read-Truth $truthPath))
    }
    finally {
        $synthesizer.Dispose()
    }
}
