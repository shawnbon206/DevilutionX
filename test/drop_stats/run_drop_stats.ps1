<#
.SYNOPSIS
Runs the drop statistics simulation in parallel worker processes and keeps them running.

.DESCRIPTION
Copies drop_stats_test.exe, its DLLs and assets into <OutDir>\bin so the build folder stays free for
rebuilding, then starts one worker per seed block and stays open as a supervisor: a worker that exits
early (a level that hangs the game's level generator makes it exit, see hung_<n>.csv) is restarted
and continues without that level. Keep this
window open until it says the run is done.

Running the script again with the same OutDir resumes every worker where it stopped, using the seed
blocks saved in run.json. Load the results with load_drop_stats.py, which can run at any time.

.EXAMPLE
.\test\drop_stats\run_drop_stats.ps1 -Hours 10

.EXAMPLE
.\test\drop_stats\run_drop_stats.ps1 -Status
#>
param(
	[string]$OutDir = (Join-Path $HOME 'drop-stats-data'),
	[int]$Workers = 20,
	[double]$Hours = 10,
	[long]$FirstSeed = 0,
	[long]$SeedsPerWorker = 100000000,
	[string]$BuildDir = (Join-Path $PSScriptRoot '..\..\build-ninja-vcpkg-relwithdebinfo'),
	[switch]$Status,
	[switch]$Stop
)

$ErrorActionPreference = 'Stop'
$runFile = Join-Path $OutDir 'run.json'

function Get-Workers {
	Get-Process -Name drop_stats_test -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$OutDir*" }
}

function Get-LatestLog([int]$worker) {
	Get-ChildItem $OutDir -Filter "worker_${worker}_*.log" | Where-Object Name -notlike '*.err.log' | Sort-Object LastWriteTime | Select-Object -Last 1
}

function Show-Status {
	Write-Output ("{0:t}  {1} worker processes running" -f (Get-Date), @(Get-Workers).Count)
	for ($i = 0; $i -lt $run.workers; $i++) {
		$log = Get-LatestLog $i
		$last = if ($log) { Get-Content $log.FullName -Tail 20 | Where-Object { $_ -match '^worker ' } | Select-Object -Last 1 }
		Write-Output ("  {0}" -f $last)
	}
	$bytes = (Get-ChildItem $OutDir -Filter 'items_*.csv' | Measure-Object Length -Sum).Sum
	$hung = @(Get-ChildItem $OutDir -Filter 'hung_*.csv' | Get-Content).Count
	Write-Output ("  items CSV total: {0:N1} GB, levels left out because they hang: {1}" -f ($bytes / 1GB), $hung)
}

if ($Stop) {
	Get-Workers | Stop-Process
	Write-Output 'Stopped. Run the script again with the same OutDir to resume.'
	return
}

if ($Status) {
	$run = Get-Content $runFile -Raw | ConvertFrom-Json
	Show-Status
	return
}

if (@(Get-Workers).Count -gt 0) {
	throw "Workers are already running for $OutDir. Use -Status, or -Stop and then start again."
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
if (Test-Path $runFile) {
	$run = Get-Content $runFile -Raw | ConvertFrom-Json
	Write-Output "Resuming run from $runFile ($($run.workers) workers, first seed $($run.firstSeed))"
} else {
	$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
	$rev = (git -C $repo rev-parse HEAD).Trim()
	if (git -C $repo status --porcelain --untracked-files=no) { $rev += '-dirty' }
	$run = [pscustomobject]@{ workers = $Workers; firstSeed = $FirstSeed; seedsPerWorker = $SeedsPerWorker; gitRev = $rev }
	$run | ConvertTo-Json | Set-Content $runFile
}

# Snapshot the binaries on the first start only, so a resumed run keeps using the same simulation.
$bin = Join-Path $OutDir 'bin'
if (-not (Test-Path (Join-Path $bin 'drop_stats_test.exe'))) {
	New-Item -ItemType Directory -Force $bin | Out-Null
	Copy-Item (Join-Path $BuildDir 'drop_stats_test.exe') $bin
	Copy-Item (Join-Path $BuildDir '*.dll') $bin
	Copy-Item -Recurse (Join-Path $BuildDir 'assets') $bin
}

$stopAt = [DateTimeOffset]::UtcNow.AddHours($Hours).ToUnixTimeSeconds()

function Start-Worker([int]$worker) {
	$env:DROPSTATS_OUT_DIR = $OutDir
	$env:DROPSTATS_WORKER = "$worker"
	$env:DROPSTATS_FIRST_SEED = "$([long]$run.firstSeed + $worker * [long]$run.seedsPerWorker)"
	$env:DROPSTATS_SEED_COUNT = "$($run.seedsPerWorker)"
	$env:DROPSTATS_STOP_AT = "$stopAt"
	$env:DROPSTATS_GIT_REV = $run.gitRev
	$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
	$start = @{
		FilePath = Join-Path $bin 'drop_stats_test.exe'
		ArgumentList = '--gtest_filter=DropStats.Record'
		WorkingDirectory = $bin
		WindowStyle = 'Hidden'
		PassThru = $true
		RedirectStandardOutput = Join-Path $OutDir "worker_${worker}_$stamp.log"
		RedirectStandardError = Join-Path $OutDir "worker_${worker}_$stamp.err.log"
	}
	$process = Start-Process @start
	$process.PriorityClass = 'BelowNormal'
	$process
}

$processes = @{}
$restarts = @{}
for ($i = 0; $i -lt $run.workers; $i++) {
	$processes[$i] = Start-Worker $i
}
Write-Output ("Started {0} workers, stopping at {1:g}. Output in {2}" -f $run.workers, [DateTimeOffset]::FromUnixTimeSeconds($stopAt).LocalDateTime, $OutDir)
Write-Output 'Keep this window open; it restarts workers that stop early. Ctrl+C here leaves the workers running without supervision.'

$lastStatus = Get-Date
while ($processes.Count -gt 0) {
	Start-Sleep -Seconds 15
	foreach ($i in @($processes.Keys)) {
		if (-not $processes[$i].HasExited) {
			continue
		}
		$finished = Get-Content (Get-LatestLog $i).FullName | Where-Object { $_ -match '^worker \d+ finished' }
		if ($finished -or [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() -ge $stopAt) {
			$processes.Remove($i)
		} elseif ($restarts[$i] -ge 20) {
			Write-Output ("{0:t}  worker {1} keeps stopping early, giving up on it; see its logs in {2}" -f (Get-Date), $i, $OutDir)
			$processes.Remove($i)
		} else {
			Write-Output ("{0:t}  worker {1} stopped early, restarting" -f (Get-Date), $i)
			$restarts[$i] = $restarts[$i] + 1
			$processes[$i] = Start-Worker $i
		}
	}
	if ((Get-Date) - $lastStatus -ge [TimeSpan]::FromMinutes(10)) {
		Show-Status
		$lastStatus = Get-Date
	}
}
Show-Status
Write-Output "Run done. Load it with: python test\drop_stats\load_drop_stats.py $OutDir"
