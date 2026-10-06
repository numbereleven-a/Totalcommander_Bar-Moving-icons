param(
    [string]$Compiler = 'g++.exe',
    [string]$Version = '1.0.0'
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw 'Use a numeric three-part version.' }
$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
$env:PATH = (Split-Path -Parent $compilerPath) + [IO.Path]::PathSeparator + $env:PATH
$output = Join-Path $PSScriptRoot ('artifacts\' + $Version + '\tests-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output | Out-Null
$flags = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0A00',
    '-static','-s',"-ffile-prefix-map=$PSScriptRoot=.","-fdebug-prefix-map=$PSScriptRoot=.")
Push-Location $PSScriptRoot
try {
    & $compilerPath @flags tests/regression.cpp -o (Join-Path $output 'regression.exe') -lcomctl32 -lgdi32
    if ($LASTEXITCODE) { throw 'Regression test compilation failed.' }
    & $compilerPath @flags -municode src/inspect.cpp -o (Join-Path $output 'inspect.exe')
    if ($LASTEXITCODE) { throw 'Inspector compilation failed.' }
    & (Join-Path $output 'regression.exe')
    if ($LASTEXITCODE) { throw 'Regression checks failed.' }
} finally { Pop-Location }
