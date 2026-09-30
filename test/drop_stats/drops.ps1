<#
.SYNOPSIS
Finds DevilutionX 1.5.5 multiplayer game seeds that drop the items you want.

.DESCRIPTION
A game's seed is the second it was created, so each day has 86,400 seeds. Simulate the days around the
day you play, load them, then search:

  drops.ps1 simulate 2026-09-30 14     Simulate 14 days starting Sep 30, in the background (about 40 min
                                       per day, days finish in order). Run it again to continue after stop.
  drops.ps1 status                     What is running and which days are done.
  drops.ps1 stop                       Stop simulating.
  drops.ps1 load                       Add finished work to the database. Safe while simulating.
  drops.ps1 find 2026-10-03 --type ring amulet --prefix "Dragon's" Gold --suffix "the Zodiac" Life
                                       Best seeds for playing on Oct 3, from games created up to 5 days
                                       before or 1 day after. More options: drops.ps1 find --help

Data goes to ~\drop-stats (one folder per day plus drops.db); set DROPSTATS_ROOT to put it elsewhere.
The simulation uses the drop_stats_test.exe from the current build, copied when simulate starts.
#>
param(
	[Parameter(Position = 0)]
	[ValidateSet('simulate', 'status', 'stop', 'load', 'find', 'supervise')]
	[string]$Command,
	[Parameter(Position = 1, ValueFromRemainingArguments)]
	[string[]]$Arguments
)

$ErrorActionPreference = 'Stop'
$Workers = 20
$Root = if ($env:DROPSTATS_ROOT) { $env:DROPSTATS_ROOT } else { Join-Path $HOME 'drop-stats' }
$Repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$BuildDir = Join-Path $Repo 'build-ninja-vcpkg-relwithdebinfo'
$Bin = Join-Path $Root 'bin'
$PidFile = Join-Path $Root 'simulate.pid'
$Log = Join-Path $Root 'simulate.log'

function Get-DayRange([string]$date) {
	$midnight = [DateTime]::ParseExact($date, 'yyyy-MM-dd', $null)
	# Daylight saving changes make some days 23 or 25 hours long.
	@(([DateTimeOffset]$midnight).ToUnixTimeSeconds(), ([DateTimeOffset]$midnight.AddDays(1)).ToUnixTimeSeconds())
}

function Get-Supervisor {
	if (-not (Test-Path $PidFile)) { return $null }
	$process = Get-Process -Id (Get-Content $PidFile) -ErrorAction SilentlyContinue
	if ($process -and $process.ProcessName -like 'pwsh*') { $process } else { $null }
}

function Get-Workers {
	Get-Process -Name drop_stats_test -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Bin*" }
}

function Write-Log([string]$message) {
	"{0:yyyy-MM-dd HH:mm}  {1}" -f (Get-Date), $message | Add-Content $Log
}

function Get-DaySeeds([string]$dayDir) {
	$rows = 0
	foreach ($file in Get-ChildItem $dayDir -Filter 'games_*.csv' -ErrorAction SilentlyContinue) {
		$rows += (Get-Content $file.FullName | Measure-Object -Line).Lines - 1
	}
	[Math]::Floor($rows / 3)
}

function Start-Worker([string]$dayDir, [long]$dayStart, [long]$seedsPerWorker, [int]$worker) {
	$env:DROPSTATS_OUT_DIR = $dayDir
	$env:DROPSTATS_WORKER = "$worker"
	$env:DROPSTATS_FIRST_SEED = "$($dayStart + $worker)"
	$env:DROPSTATS_SEED_STEP = "$Workers"
	$env:DROPSTATS_SEED_COUNT = "$seedsPerWorker"
	$env:DROPSTATS_GIT_REV = (Get-Content (Join-Path $Bin 'git_rev.txt'))
	$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
	$start = @{
		FilePath = Join-Path $Bin 'drop_stats_test.exe'
		ArgumentList = '--gtest_filter=DropStats.Record'
		WorkingDirectory = $Bin
		WindowStyle = 'Hidden'
		PassThru = $true
		RedirectStandardOutput = Join-Path $dayDir "worker_${worker}_$stamp.log"
		RedirectStandardError = Join-Path $dayDir "worker_${worker}_$stamp.err.log"
	}
	$process = Start-Process @start
	$process.PriorityClass = 'BelowNormal'
	$process
}

# Runs every worker on one day and restarts workers that exit early: a level that hangs the game's
# level generator makes a worker exit, and on restart it continues without that level (hung_<n>.csv).
function Invoke-Day([string]$date) {
	$dayDir = Join-Path $Root $date
	if (Test-Path (Join-Path $dayDir 'done')) { return }
	New-Item -ItemType Directory -Force $dayDir | Out-Null
	$range = Get-DayRange $date
	$seedsPerWorker = [Math]::Ceiling(($range[1] - $range[0]) / $Workers)
	Write-Log "simulating $date"
	$processes = @{}
	$restarts = @{}
	for ($i = 0; $i -lt $Workers; $i++) {
		$processes[$i] = Start-Worker $dayDir $range[0] $seedsPerWorker $i
	}
	while ($processes.Count -gt 0) {
		Start-Sleep -Seconds 10
		foreach ($i in @($processes.Keys)) {
			if (-not $processes[$i].HasExited) { continue }
			$log = Get-ChildItem $dayDir -Filter "worker_${i}_*.log" | Where-Object Name -notlike '*.err.log' | Sort-Object LastWriteTime | Select-Object -Last 1
			if (Get-Content $log.FullName | Where-Object { $_ -match '^worker \d+ finished' }) {
				$processes.Remove($i)
			} elseif ($restarts[$i] -ge 20) {
				Write-Log "worker $i on $date keeps stopping early, giving up on it; see its logs in $dayDir"
				$processes.Remove($i)
			} else {
				Write-Log "worker $i on $date stopped early (a level that hangs is being skipped), restarting"
				$restarts[$i] = $restarts[$i] + 1
				$processes[$i] = Start-Worker $dayDir $range[0] $seedsPerWorker $i
			}
		}
	}
	Set-Content (Join-Path $dayDir 'done') (Get-Date -Format 'yyyy-MM-dd HH:mm')
	Write-Log "finished $date"
}

switch ($Command) {
	'simulate' {
		if ($Arguments.Count -lt 1) { throw 'Usage: drops.ps1 simulate <yyyy-MM-dd> [days]' }
		$first = [DateTime]::ParseExact($Arguments[0], 'yyyy-MM-dd', $null)
		$days = if ($Arguments.Count -ge 2) { [int]$Arguments[1] } else { 1 }
		if (Get-Supervisor) { throw "Already simulating. Use 'drops.ps1 status', or 'drops.ps1 stop' first." }
		if (Get-Workers) { throw "Workers from an earlier run are still running. Use 'drops.ps1 stop' first." }
		New-Item -ItemType Directory -Force $Root, $Bin | Out-Null
		Copy-Item (Join-Path $BuildDir 'drop_stats_test.exe') $Bin -Force
		Copy-Item (Join-Path $BuildDir '*.dll') $Bin -Force
		Copy-Item -Recurse -Force (Join-Path $BuildDir 'assets') $Bin
		$rev = (git -C $Repo rev-parse HEAD).Trim()
		if (git -C $Repo status --porcelain --untracked-files=no) { $rev += '-dirty' }
		Set-Content (Join-Path $Bin 'git_rev.txt') $rev
		$dates = @(for ($d = 0; $d -lt $days; $d++) { $first.AddDays($d).ToString('yyyy-MM-dd') })
		$supervisor = Start-Process pwsh -ArgumentList (@('-NoProfile', '-File', $PSCommandPath, 'supervise') + $dates) -WindowStyle Hidden -PassThru
		Set-Content $PidFile $supervisor.Id
		Write-Output ("Simulating {0} to {1} in the background, about 40 minutes per day." -f $dates[0], $dates[-1])
		Write-Output "Check on it with 'drops.ps1 status'; you can close this window."
	}
	'supervise' {
		foreach ($date in $Arguments) { Invoke-Day $date }
		Remove-Item $PidFile -ErrorAction SilentlyContinue
		Write-Log 'all requested days finished'
	}
	'stop' {
		$supervisor = Get-Supervisor
		if ($supervisor) { Stop-Process -Id $supervisor.Id }
		Get-Workers | Stop-Process
		Remove-Item $PidFile -ErrorAction SilentlyContinue
		Write-Output "Stopped. Run the same 'drops.ps1 simulate' again to continue where it left off."
	}
	'status' {
		$running = [bool](Get-Supervisor)
		Write-Output ("Simulation: {0}, {1} worker processes" -f ($(if ($running) { 'running' } else { 'not running' })), @(Get-Workers).Count)
		foreach ($dayDir in Get-ChildItem $Root -Directory -ErrorAction SilentlyContinue | Where-Object Name -match '^\d{4}-\d{2}-\d{2}$' | Sort-Object Name) {
			$range = Get-DayRange $dayDir.Name
			$total = $range[1] - $range[0]
			$hung = @(Get-ChildItem $dayDir.FullName -Filter 'hung_*.csv' | Get-Content).Count
			$state = if (Test-Path (Join-Path $dayDir.FullName 'done')) { 'done' } else { '{0:P0}' -f ((Get-DaySeeds $dayDir.FullName) / $total) }
			Write-Output ("  {0}  {1}{2}" -f $dayDir.Name, $state, $(if ($hung) { ", $hung level(s) skipped because they hang" } else { '' }))
		}
		if (Test-Path $Log) {
			Write-Output 'Recent events:'
			Get-Content $Log -Tail 5 | ForEach-Object { "  $_" }
		}
	}
	'load' {
		$dayDirs = @(Get-ChildItem $Root -Directory -ErrorAction SilentlyContinue | Where-Object Name -match '^\d{4}-\d{2}-\d{2}$' | Sort-Object Name | ForEach-Object FullName)
		if (-not $dayDirs) { throw "Nothing simulated yet in $Root." }
		python (Join-Path $PSScriptRoot 'load_drop_stats.py') @dayDirs --db (Join-Path $Root 'drops.db')
	}
	'find' {
		$db = Join-Path $Root 'drops.db'
		$query = Join-Path $PSScriptRoot 'query_drop_stats.py'
		if ($Arguments.Count -ge 1 -and $Arguments[0] -match '^\d{4}-\d{2}-\d{2}$') {
			python $query $db --day $Arguments[0] --before 5 --after 1 @($Arguments | Select-Object -Skip 1)
		} else {
			python $query $db @Arguments
		}
	}
	default {
		Get-Help $PSCommandPath -Detailed
	}
}
