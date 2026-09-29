# Aurora build script
param(
  [switch]$Release,
  [switch]$Perft,
  [switch]$Bench,
  [switch]$DumpFen,
  [int]$Nodes = 0
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$gpp  = Join-Path $root '..\mingw\mingw64\bin\g++.exe'
if (-not (Test-Path $gpp)) { throw "compiler not found at $gpp" }

$src = @(
  'src\bitboard.cpp',
  'src\position.cpp',
  'src\movegen.cpp',
  'src\eval.cpp',
  'src\tt.cpp',
  'src\search.cpp',
  'src\uci.cpp',
  'src\main.cpp'
) | ForEach-Object { Join-Path $root $_ }

$outDir = Join-Path $root 'bin'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out = Join-Path $outDir 'aurora.exe'

$flags = @(
  '-O3', '-std=c++20', '-march=native', '-funroll-loops',
  '-fno-exceptions', '-fno-rtti', '-DNDEBUG',
  '-Wall', '-Wextra', '-Wno-unused-parameter'
)
if (-not $Release) { $flags = @('-O1','-g','-std=c++20','-march=native','-Wall','-Wextra','-Wno-unused-parameter') }

$sw = [Diagnostics.Stopwatch]::StartNew()
& $gpp @flags '-Isrc' @src -o $out
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
$sw.Stop()
"built $out in $([math]::Round($sw.Elapsed.TotalSeconds,1))s"

if ($Perft)  { & $out perft $Nodes }
if ($Bench)  { & $out bench }
if ($DumpFen){ & $out dumpfen }
