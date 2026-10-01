<#
.SYNOPSIS
Finds DevilutionX 1.5.5 multiplayer game seeds that drop the items you want. Run with --help for usage.
#>
param(
	[Parameter(Position = 0)]
	[string]$Command,
	[Parameter(Position = 1, ValueFromRemainingArguments)]
	[string[]]$Arguments
)

$ErrorActionPreference = 'Stop'

$Usage = @'
Finds DevilutionX 1.5.5 multiplayer game seeds that drop the items you want.
A game's seed is the second it was created. The quest settings come from your diablo.ini; the host must
have the same ones when creating the game (Diablo mode, full quests, Theo and Cow quests off).

Searches seeds from right now on, printing matching games as they are found:
  drops.ps1 search <minutes> <wishlist>
  drops.ps1 search 5 --type ring amulet --prefix Obsidian Gold "Dragon's" --suffix life "the zodiac" --min-roll 80
  <minutes> 0 searches until Ctrl+C, e.g. in the background while you play. Ctrl+C always stops
  and prints the best seeds. About 39 seeds a second on 20 workers; --workers 10 leaves more CPU free.

Wishlist options:
  --type ring amulet ...     sword axe mace bow staff helm shield light_armor medium_armor heavy_armor
  --base Maul "Great Axe"    base items
  --prefix "King's" Gold     prefixes; the item's prefix must be one of them
  --suffix haste life ...    suffixes; "of " is optional. With both lists the item needs both,
  --either                   ...or with --either, one of the two is enough
  --unique "Harlequin Crest" unique items
  --min-roll 80              each wanted affix rolled at least 80% of the way up its own range
  Name:N                     a minimum for one affix's first shown number, e.g. --prefix Obsidian:39
  --difficulty 0|1|2         Normal, Nightmare, Hell (default: all three)
  --seeds 50                 how many of the best seeds to list (default 20)
  --ini <path>               simulate with another diablo.ini's quest settings instead of yours
Names are not case-sensitive; quote names with spaces or apostrophes. More: drops.ps1 search --help

Uses the current build's drop_stats_test.exe, copied to ~\drop-stats\search-bin (set DROPSTATS_ROOT to move it).
'@

if (-not $Command -or $Command -in 'help', '--help', '-help', '/?') {
	Write-Output $Usage
	return
}
if ($Command -ne 'search') {
	if ($Command.StartsWith('--')) {
		Write-Output "The command goes first, before the wishlist: drops.ps1 search <minutes> $Command ..."
	} else {
		Write-Output "Unknown command '$Command'. The command is search; see drops.ps1 --help."
	}
	exit 1
}
if ($Arguments -contains '--help') {
	python (Join-Path $PSScriptRoot 'search_drop_stats.py') --help
	return
}

# Quest settings change the levels, so the simulation follows the host's diablo.ini: the one this PC's game
# uses, or another one given with --ini.
$Ini = Join-Path $env:APPDATA 'diasurgical\devilution\diablo.ini'
$iniAt = [Array]::IndexOf([string[]]@($Arguments), '--ini')
if ($iniAt -ge 0) {
	if ($iniAt + 1 -ge $Arguments.Count) { throw '--ini needs the path of a diablo.ini.' }
	$Ini = $Arguments[$iniAt + 1]
	$Arguments = @($Arguments | Select-Object -First $iniAt) + @($Arguments | Select-Object -Skip ($iniAt + 2))
}
if (-not (Test-Path $Ini)) { throw "$Ini not found." }
function Get-IniValue([string]$section, [string]$key, [string]$default) {
	$current = ''
	foreach ($line in Get-Content $Ini -ErrorAction SilentlyContinue) {
		if ($line -match '^\[(.+)\]\s*$') {
			$current = $Matches[1]
		} elseif ($current -eq $section -and $line -match "^$([regex]::Escape($key))=(.*)$") {
			return $Matches[1].Trim()
		}
	}
	$default
}
# The simulator was checked against real drops in Diablo mode with full quests only.
if ((Get-IniValue 'GameMode' 'Game' '0') -ne '2') { throw "The search only simulates Diablo mode (Game=2 under [GameMode]); $Ini says otherwise." }
if ((Get-IniValue 'Game' 'MultiplayerFullQuests' '0') -ne '1') { throw "The search only simulates games with full quests (MultiplayerFullQuests=1); $Ini says otherwise." }
$env:DROPSTATS_RANDOMIZE_QUESTS = Get-IniValue 'Game' 'Randomize Quests' '1'
$env:DROPSTATS_THEO_QUEST = Get-IniValue 'Game' 'Theo Quest' '0'
$env:DROPSTATS_COW_QUEST = Get-IniValue 'Game' 'Cow Quest' '0'
$onOff = { param($value) if ($value -eq '1') { 'on' } else { 'off' } }
Write-Output ("Settings from {0}: Diablo mode, full quests, randomized quests {1}, Theo quest {2}, Cow quest {3}. Host with the same." -f $Ini, (& $onOff $env:DROPSTATS_RANDOMIZE_QUESTS), (& $onOff $env:DROPSTATS_THEO_QUEST), (& $onOff $env:DROPSTATS_COW_QUEST))

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

$searchArgs = @($Arguments)
if ($searchArgs.Count -ge 1 -and $searchArgs[0] -match '^\d+(\.\d+)?$') {
	$searchArgs = @('--minutes', $searchArgs[0]) + @($searchArgs | Select-Object -Skip 1)
}
python (Join-Path $PSScriptRoot 'search_drop_stats.py') --bin $SearchBin @searchArgs
