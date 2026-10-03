<#
.SYNOPSIS
Finds DevilutionX 1.5.5 multiplayer game seeds that drop the items you want. Run with --help for usage.
#>
param(
	[Parameter(ValueFromRemainingArguments)]
	[string[]]$Arguments
)

$ErrorActionPreference = 'Stop'
$Search = Join-Path $PSScriptRoot 'search_drop_stats.py'

# "search" used to be the command; it is the only thing this script does now.
$Arguments = @($Arguments | Where-Object { $_ -ne $null })
if ($Arguments.Count -ge 1 -and $Arguments[0] -eq 'search') {
	$Arguments = @($Arguments | Select-Object -Skip 1)
}
if ($Arguments.Count -eq 0 -or $Arguments -contains '--help' -or $Arguments -contains 'help' -or $Arguments -contains '/?') {
	python $Search --bin . --help
	return
}

$Root = if ($env:DROPSTATS_ROOT) { $env:DROPSTATS_ROOT } else { Join-Path $HOME 'drop-stats' }
$BuildDir = Join-Path $PSScriptRoot '..\..\build-ninja-vcpkg-relwithdebinfo'
$SearchBin = Join-Path $Root 'search-bin'

if (Get-Process -Name drop_stats_test -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$SearchBin*" }) {
	throw 'Another search is still running.'
}

# Copy the simulator so the build folder stays free for rebuilding while a search runs.
New-Item -ItemType Directory -Force $SearchBin | Out-Null
Copy-Item (Join-Path $BuildDir 'drop_stats_test.exe') $SearchBin -Force
Copy-Item (Join-Path $BuildDir '*.dll') $SearchBin -Force
Copy-Item -Recurse -Force (Join-Path $BuildDir 'assets') $SearchBin
$env:DROPSTATS_NAMES_FILE = Join-Path $SearchBin 'names.csv'
& (Join-Path $SearchBin 'drop_stats_test.exe') --gtest_filter=DropStats.DumpNames | Out-Null
Remove-Item Env:DROPSTATS_NAMES_FILE

python $Search --bin $SearchBin @Arguments
