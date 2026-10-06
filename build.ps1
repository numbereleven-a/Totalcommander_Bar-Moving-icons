param(
    [string]$Compiler = 'g++.exe',
    [string]$Version = '1.0.0'
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw 'Use a numeric three-part version.' }
$versionParts = @($Version.Split('.') | ForEach-Object { [int]$_ })
if ($versionParts | Where-Object { $_ -gt 65535 }) { throw 'Version components must not exceed 65535.' }
$numericVersion = ($versionParts + @(0)) -join ','
$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
$compilerDirectory = Split-Path -Parent $compilerPath
$env:PATH = $compilerDirectory + [IO.Path]::PathSeparator + $env:PATH
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$output = Join-Path $PSScriptRoot "artifacts\$Version\$stamp"
New-Item -ItemType Directory -Path $output | Out-Null

Add-Type -AssemblyName System.Drawing
foreach ($open in @($false, $true)) {
    $bitmap = [Drawing.Bitmap]::new(32, 32)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([Drawing.Color]::Transparent)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $pen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(255, 55, 60, 65), 3)
    $body = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255, 212, 141, 0))
    if ($open) { $graphics.DrawArc($pen, 14, 3, 12, 14, 180, 210) }
    else { $graphics.DrawArc($pen, 9, 3, 14, 17, 180, 180); $graphics.DrawLine($pen, 9, 10, 9, 17); $graphics.DrawLine($pen, 23, 10, 23, 17) }
    $graphics.FillRectangle($body, 6, 15, 20, 14)
    $graphics.DrawRectangle($pen, 6, 15, 20, 14)
    $graphics.FillEllipse([Drawing.Brushes]::Black, 14, 19, 4, 4)
    $graphics.FillRectangle([Drawing.Brushes]::Black, 15, 21, 2, 4)
    # Store a 32-bit PNG image in the ICO container. Icon.Save on Windows
    # PowerShell can reduce an icon created from a handle to a 16-color image.
    $imageStream = [IO.MemoryStream]::new()
    $bitmap.Save($imageStream, [Drawing.Imaging.ImageFormat]::Png)
    $imageBytes = $imageStream.ToArray()
    $file = if ($open) { 'unlocked.ico' } else { 'locked.ico' }
    $stream = [IO.File]::Create((Join-Path $output $file))
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]1)
        $writer.Write([byte]32); $writer.Write([byte]32); $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$imageBytes.Length); $writer.Write([uint32]22)
        $writer.Write($imageBytes)
    } finally {
        $writer.Dispose(); $imageStream.Dispose(); $body.Dispose(); $pen.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
    }
}
Push-Location $output
try {
    foreach ($extension in @('dll', 'exe')) {
        $fileType = if ($extension -eq 'dll') { 'VFT_DLL' } else { 'VFT_APP' }
        $description = if ($extension -eq 'dll') { 'Button movement extension' } else { 'Button movement launcher' }
        $resource = @"
#include <windows.h>
1 ICON "locked.ico"
2 ICON "unlocked.ico"
1 VERSIONINFO
 FILEVERSION $numericVersion
 PRODUCTVERSION $numericVersion
 FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
 FILEFLAGS 0
 FILEOS VOS_NT_WINDOWS32
 FILETYPE $fileType
 FILESUBTYPE 0
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904B0"
  BEGIN
   VALUE "FileDescription", "$description\0"
   VALUE "FileVersion", "$Version\0"
   VALUE "InternalName", "TcBarMove\0"
   VALUE "OriginalFilename", "TcBarMove.$extension\0"
   VALUE "ProductName", "Button movement\0"
   VALUE "ProductVersion", "$Version\0"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x0409, 1200
 END
END
"@
        Set-Content -LiteralPath "$extension.rc" -Value $resource -Encoding ascii
        & (Join-Path $compilerDirectory 'windres.exe') "$extension.rc" -O coff -o "$extension.o"
        if ($LASTEXITCODE) { throw "Resource compilation failed for $extension." }
    }
} finally { Pop-Location }
$flags = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0A00',
    '-static','-static-libgcc','-static-libstdc++','-s',"-ffile-prefix-map=$PSScriptRoot=.","-fdebug-prefix-map=$PSScriptRoot=.",'-Wl,--no-insert-timestamp')
Push-Location $PSScriptRoot
try {
    & $compilerPath @flags -shared src/extension.cpp (Join-Path $output 'dll.o') -o (Join-Path $output 'TcBarMove.dll') -lcomctl32 -lgdi32
    if ($LASTEXITCODE) { throw 'Extension compilation failed.' }
    & $compilerPath @flags -municode -mwindows src/launcher.cpp (Join-Path $output 'exe.o') -o (Join-Path $output 'TcBarMove.exe') -lshell32
    if ($LASTEXITCODE) { throw 'Launcher compilation failed.' }
} finally { Pop-Location }
$files = @('TcBarMove.exe', 'TcBarMove.dll')
foreach ($file in $files) {
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $output $file))
    if ($info.FileVersion -ne $Version -or $info.ProductVersion -ne $Version -or $info.OriginalFilename -ne $file) {
        throw "Version resource validation failed for $file."
    }
}
$testDirectory = Join-Path $PSScriptRoot 'test-totalcmd'
if (Test-Path -LiteralPath $testDirectory -PathType Container) {
    # Check both destinations before replacing either file: a loaded DLL
    # remains pinned until the test Total Commander exits.
    foreach ($file in $files) {
        $target = Join-Path $testDirectory $file
        if (Test-Path -LiteralPath $target) {
            try {
                $stream = [IO.File]::Open($target, [IO.FileMode]::Open, [IO.FileAccess]::Write,
                    [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
                $stream.Dispose()
            } catch {
                throw 'Cannot update the test files. Close the test Total Commander if the add-on is loaded, then run the build again.'
            }
        }
    }
    foreach ($file in $files) {
        Copy-Item -LiteralPath (Join-Path $output $file) -Destination (Join-Path $testDirectory $file) -Force
    }
}
Write-Output $output
