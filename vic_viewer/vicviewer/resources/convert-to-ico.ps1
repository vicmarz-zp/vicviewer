# Script para convertir PNG a ICO
# Uso: .\convert-to-ico.ps1 -InputPng "imagen.png" -OutputIco "vicviewer.ico"

param(
    [string]$InputPng = "vicviewer.png",
    [string]$OutputIco = "vicviewer.ico"
)

Add-Type -AssemblyName System.Drawing

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$inputPath = Join-Path $scriptDir $InputPng
$outputPath = Join-Path $scriptDir $OutputIco

if (-not (Test-Path $inputPath)) {
    Write-Error "No se encontró: $inputPath"
    exit 1
}

# Cargar imagen original
$original = [System.Drawing.Image]::FromFile($inputPath)

# Tamaños de icono estándar
$sizes = @(256, 128, 64, 48, 32, 16)

# Crear archivo ICO manualmente
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)

# Header ICO
$bw.Write([Int16]0)        # Reserved
$bw.Write([Int16]1)        # Type (1 = ICO)
$bw.Write([Int16]$sizes.Count)  # Number of images

# Calcular offset inicial (header + directory entries)
$offset = 6 + ($sizes.Count * 16)

$imageData = @()
foreach ($size in $sizes) {
    # Crear bitmap redimensionado
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($original, 0, 0, $size, $size)
    $g.Dispose()
    
    # Convertir a PNG
    $pngMs = New-Object System.IO.MemoryStream
    $bmp.Save($pngMs, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBytes = $pngMs.ToArray()
    $pngMs.Dispose()
    $bmp.Dispose()
    
    $imageData += ,@{Size=$size; Data=$pngBytes}
}

# Escribir directory entries
foreach ($img in $imageData) {
    $bw.Write([Byte]$(if($img.Size -eq 256){0}else{$img.Size}))  # Width
    $bw.Write([Byte]$(if($img.Size -eq 256){0}else{$img.Size}))  # Height
    $bw.Write([Byte]0)         # Color palette
    $bw.Write([Byte]0)         # Reserved
    $bw.Write([Int16]1)        # Color planes
    $bw.Write([Int16]32)       # Bits per pixel
    $bw.Write([Int32]$img.Data.Length)  # Image size
    $bw.Write([Int32]$offset)  # Offset
    $offset += $img.Data.Length
}

# Escribir image data
foreach ($img in $imageData) {
    $bw.Write($img.Data)
}

$original.Dispose()

# Guardar archivo
[System.IO.File]::WriteAllBytes($outputPath, $ms.ToArray())
$bw.Dispose()
$ms.Dispose()

Write-Host "✅ Icono creado: $outputPath"
Write-Host "   Tamaños incluidos: $($sizes -join ', ')px"
