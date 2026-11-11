# Watch-KytyBuilds.ps1 - v8 (2026-11-11)
# Watches D:\Downloads for new KyTy builds and auto-runs the NHL 27 test
# (Test-KytyBuild.ps1) for each one. Meant to run on a schedule (every
# 15 min) via a Windows Scheduled Task. Results are copied to -UploadDir
# (a Google Drive folder) when set, so they sync and can be reviewed
# without anyone sending them.
#
# v8 complete-copy gate (2026-11-11): the 1216 and 1516 result folders
# reached Drive WITHOUT their nav shots, 04-versus.png, 05-ice.png, or
# any frame.txt - only the late flash shots and transcript made it.
# The old rule deleted the local folder as soon as the Drive copy held
# transcript.txt alone (backfill) or 04-versus.png alone (post-run), so
# a partial copy looked complete and the local evidence was lost. Now
# the local folder is only deleted when the Drive copy holds the full
# key set (transcript.txt, 04-versus.png, 05-ice.png, 07-pause-menu.png),
# and the post-run copy waits 3 minutes for the Drive client to upload
# before that check. A partial copy keeps its local folder, and the next
# pass's backfill retries it.
#
# v7 versioned markers (user's call, 2026-11-11): v6's tested-<tag>.txt
# blocked re-running a build forever unless the marker was deleted by
# hand, which got in the way of re-testing after a script fix. The
# marker now stores the test script version it was tested under, and a
# build is only "done" if its marker version is at least the current
# script version. When the script version increases, every build gets
# re-tested once automatically. Old v6 markers (date only, no version)
# count as version 0, so they re-test once under the new scheme.
#
# v6 efficiency fixes (the v5 loop wasted whole runs):
#  - LOCAL MARKER FILES. v5 decided "done" only from result folders in
#    Downloads/Drive, so when the user trashed the Drive result folders
#    (cleanup 2026-11-10) the watcher forgot #139 was tested and
#    re-downloaded it for another atlas capture. v6 writes a small marker
#    file in this folder when a run is conclusive, and checks the marker
#    first: atlas-done.txt (atlas capture succeeded), and
#    tested-<tag>.txt (that build reached the game). Trashing Drive
#    folders no longer causes a re-run. v7: the marker stores the test
#    script version, so a script update re-tests builds automatically;
#    a marker is only "done" when its version is at least the current
#    script version. Delete a marker to force a re-run under the same
#    script version.
#  - SORT BY RELEASE DATE, NOT FOLDER TIME. v5 sorted build folders by
#    LastWriteTime, so a just re-downloaded older build (#139) sorted as
#    "newest" and got tested before the actually-newer #146. v6 parses
#    the YYYY-MM-DD out of the tag in the folder name and sorts by that.
#  - SELF-UPDATE. v6 downloads the latest Watch-KytyBuilds.ps1 from the
#    kyty-test branch before each pass (same as the test script). If a
#    newer copy arrives, it replaces this file and exits so the next
#    scheduled tick runs the new version. No more manual watcher swaps.
#    If the download fails, it just carries on with the current copy.
#
# v5 made the atlas capture AUTOMATIC (no flag file, no manual run). The
# atlas-dump build (#139, 4b197fb) holds the evidence for the versus
# corruption, but three manual flag-file runs (1448, 1458, 1510) all missed
# versus and the user had to drop the flag and run the watcher by hand
# each time - then closed the window early on the third one. v5 checks on
# every pass: if no atlas result folder anywhere (local captures or Drive)
# holds a *_smp_*.bin file, and fewer than 5 atlas attempts exist, it runs
# the atlas capture on #139 by itself, then carries on with the normal
# newest-build pass. A successful capture (bins present) stops the retries.
# The atlas-request.txt flag still works as an override for any other tag.
#
# What is different from the v1 watcher:
#  - No Test-KytyBuild.game.txt requirement. The v3 test script carries the
#    game path as a default, so the watcher no longer refuses to run when
#    the game-path file is missing (that file was deleted in the fresh start
#    and the watcher would have exited on every pass).
#  - No tested list (kept): a build is "done" only when a Downloads result
#    folder for it contains 04-versus.png. A run that stalls at the title
#    screen leaves only nav shots, so it is NOT done and gets re-tested
#    automatically on the next pass.
#  - The test script is self-updated from the kyty-test branch before every
#    run, so script fixes flow automatically.
#
# Usage: run via Run-Watcher.bat, or install the scheduled task with
# Install-KytyTestWatcher.ps1 (every 15 min).
param(
    [string]$BuildsRoot = "D:\Downloads",
    [string]$TestScript = "",
    [string]$UploadDir = ""
)
$ErrorActionPreference = "Stop"
$ScriptDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ScriptDir)) { $ScriptDir = (Get-Location).Path }
# Result folders (screenshots + log) live in D:\Downloads\captures, next to
# the frame dumps, so all test output is in one place.
$ResultsRoot = Join-Path $BuildsRoot "captures"
if ([string]::IsNullOrWhiteSpace($TestScript)) { $TestScript = Join-Path $ScriptDir "Test-KytyBuild.ps1" }
$LockFile = Join-Path $ScriptDir "Watch-KytyBuilds.lock"

# Auto-detect the Google Drive upload folder when -UploadDir was not passed
# (e.g. run manually via Run-Watcher.bat). The scheduled task passes it
# explicitly; a manual run does not, so without this the results never leave
# Downloads. Searches each drive for a "My Drive" folder.
if ([string]::IsNullOrWhiteSpace($UploadDir)) {
    foreach ($drv in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        $cand = Join-Path ($drv.Root) "My Drive"
        if (Test-Path $cand) { $UploadDir = Join-Path $cand "KyTyTestResults"; break }
    }
    if (-not [string]::IsNullOrWhiteSpace($UploadDir)) { Write-Host ("Upload dir auto-detected: " + $UploadDir) }
}

# If a test is already running, do nothing. A lock is stale only if the
# process that created it is no longer running (the watcher was killed or
# crashed). v6 REMOVED the old 45-minute age rule: a full test runs about
# 100 minutes, so the age rule marked the lock stale mid-run and a later
# scheduled tick could start a duplicate run while the first was still
# going. The owner-PID check alone is enough - a live owner means a live
# run, and the emulator check below is the backstop.
if (Test-Path $LockFile) {
    $lockPid = $null
    try { $lockPid = [int](Get-Content $LockFile -ErrorAction Stop | Select-Object -First 1) } catch { }
    $stale = $false
    if ($lockPid) {
        $owner = Get-Process -Id $lockPid -ErrorAction SilentlyContinue
        if (-not $owner) { $stale = $true }
    } else {
        $stale = $true
    }
    if ($stale) {
        Write-Host "Removing stale lock file."
        Remove-Item $LockFile -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host "A test is already running (lock file present). Exiting."; exit 0
    }
}
# If the user is already playing, do not start a test over their game.
if (Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue) {
    Write-Host "Emulator already running (user playing). Exiting."; exit 0
}
if (-not (Test-Path $TestScript)) { Write-Host ("Test script not found: " + $TestScript); exit 0 }
if (-not (Test-Path $BuildsRoot)) { Write-Host ("Builds folder not found: " + $BuildsRoot); exit 0 }

# Self-update the watcher itself (v6): pull the latest Watch-KytyBuilds.ps1
# from the kyty-test branch. If a newer copy arrives, replace this file
# and exit so the next scheduled tick runs the new version. This runs
# only when no test is in progress (the lock and emulator checks above
# already passed), so it never swaps the watcher mid-run. If the download
# fails, carry on with the current copy.
try {
    $watcherUrl = "https://raw.githubusercontent.com/duy64/KytyPS5-nhl/kyty-test/Watch-KytyBuilds.ps1?cb=" + [DateTime]::UtcNow.Ticks
    $watcherTmp = Join-Path $ScriptDir "Watch-KytyBuilds.new.ps1"
    Invoke-WebRequest -Uri $watcherUrl -OutFile $watcherTmp -UseBasicParsing -TimeoutSec 60 -ErrorAction Stop
    $newBytes = Get-Item $watcherTmp -ErrorAction Stop
    $curBytes = Get-Item $PSCommandPath -ErrorAction SilentlyContinue
    if ($newBytes.Length -gt 1000 -and (-not $curBytes -or $newBytes.Length -ne $curBytes.Length -or (Get-FileHash $watcherTmp -Algorithm SHA256).Hash -ne (Get-FileHash $PSCommandPath -Algorithm SHA256).Hash)) {
        Move-Item $watcherTmp $PSCommandPath -Force -ErrorAction Stop
        Write-Host "Watcher updated to the latest version. Exiting so the next tick runs the new copy."
        exit 0
    } else {
        Remove-Item $watcherTmp -Force -ErrorAction SilentlyContinue
        Write-Host "Watcher is already the latest version."
    }
} catch {
    Write-Host ("Watcher self-update skipped (could not download latest): " + $_.Exception.Message)
    if (Test-Path (Join-Path $ScriptDir "Watch-KytyBuilds.new.ps1")) { Remove-Item (Join-Path $ScriptDir "Watch-KytyBuilds.new.ps1") -Force -ErrorAction SilentlyContinue }
}

# A build is "done" only when a test actually reached the game (a result
# folder with 04-versus.png). A run that stalls at the title screen
# produces only nav shots, so it is NOT done and gets re-tested next pass.
# v6 also checks a local marker file (tested-<tag>.txt in this folder),
# written after a conclusive run, so trashing the Drive result folders
# does not make the watcher forget a build was tested. v7: the marker
# stores the test script version, and a build is only done when its
# marker version is at least the current script version, so a script
# update re-tests builds automatically. A v6 marker (date only, no
# version) counts as version 0 and re-tests once.
function Test-HasConclusiveResult {
    param([string]$BuildName)
    $marker = Join-Path $ScriptDir ("tested-" + $BuildName + ".txt")
    if (Test-Path $marker) {
        $markedVersion = 0
        try {
            $content = Get-Content $marker -Raw -ErrorAction Stop
            if ($content -match 'version\s*=\s*(\d+)') { $markedVersion = [int]$Matches[1] }
        } catch { $markedVersion = 0 }
        if ($markedVersion -ge $script:TestScriptVersion) { return $true }
    }
    $folders = Get-ChildItem -Path $ResultsRoot -Directory -Filter ("KyTyTest-" + $BuildName + "-*") -ErrorAction SilentlyContinue
    foreach ($f in $folders) {
        if (Test-Path (Join-Path $f.FullName "04-versus.png")) { return $true }
    }
    return $false
}
function Set-TestedMarker {
    param([string]$BuildName)
    $marker = Join-Path $ScriptDir ("tested-" + $BuildName + ".txt")
    Set-Content -Path $marker -Value ("version=" + $script:TestScriptVersion + " " + (Get-Date -Format "yyyy-MM-dd HH:mm")) -Encoding ASCII -ErrorAction SilentlyContinue
}

# Release date from a build folder/tag name: the tag is always
# KytyPS5-YYYY-MM-DD-... so the date is the 2nd dash-separated part.
# v6 sorts builds by this date (newest first), not by the folder
# LastWriteTime, so a just re-downloaded older build does not jump
# ahead of a newer one. Returns $null when the date cannot be parsed.
function Get-BuildDate {
    param([string]$Name)
    if ($Name -match "KytyPS5-(\d{4}-\d{2}-\d{2})") {
        try { return [datetime]::ParseExact($matches[1], "yyyy-MM-dd", $null) } catch { return $null }
    }
    return $null
}

# Download and extract one release by tag if its folder is missing.
# Returns the build folder path, or "" when the download failed. Only
# downloads releases from duy64/KytyPS5-nhl. Used by the atlas-request
# flag (a specific older build, e.g. the atlas-dump build).
function Get-BuildFolderForTag {
    param([string]$Tag)
    $folderName = $Tag + "-Windows-x64"
    $folderPath = Join-Path $BuildsRoot $folderName
    if (Test-Path (Join-Path $folderPath "kyty_emulator.exe")) { return $folderPath }
    $zipName = $folderName + ".zip"
    $zipPath = Join-Path $BuildsRoot $zipName
    $dlUrl = "https://github.com/duy64/KytyPS5-nhl/releases/download/" + $Tag + "/" + $zipName
    Write-Host ("Atlas request: downloading " + $Tag)
    try {
        Invoke-WebRequest -Uri $dlUrl -OutFile $zipPath -UseBasicParsing -TimeoutSec 600 -ErrorAction Stop
    } catch {
        Write-Host ("Atlas request download failed: " + $_.Exception.Message)
        return ""
    }
    $zipInfo = Get-Item $zipPath -ErrorAction SilentlyContinue
    if (-not $zipInfo -or $zipInfo.Length -lt 1000000) {
        Write-Host ("Atlas request download failed or too small (" + $zipInfo.Length + " bytes). Deleting bad zip.")
        Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
        return ""
    }
    Write-Host ("Extracting: " + $zipName)
    $tmpDir = Join-Path $BuildsRoot ("_extract_" + $folderName)
    if (Test-Path $tmpDir) { Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue }
    try {
        Expand-Archive -Path $zipPath -DestinationPath $tmpDir -Force -ErrorAction Stop
    } catch {
        Write-Host ("Atlas request extract failed: " + $_.Exception.Message)
        Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
        return ""
    }
    if (-not (Test-Path $folderPath)) { New-Item -ItemType Directory -Force -Path $folderPath | Out-Null }
    $top = @(Get-ChildItem -Path $tmpDir -Force -ErrorAction SilentlyContinue)
    $singleDir = $null
    if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $singleDir = $top[0] }
    if ($singleDir) {
        Get-ChildItem -Path $singleDir.FullName -Force | ForEach-Object { Move-Item $_.FullName (Join-Path $folderPath $_.Name) -Force }
    } else {
        Get-ChildItem -Path $tmpDir -Force | ForEach-Object { Move-Item $_.FullName (Join-Path $folderPath $_.Name) -Force }
    }
    Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path (Join-Path $folderPath "kyty_emulator.exe"))) {
        Write-Host "Atlas request: extracted folder has no kyty_emulator.exe."
        return ""
    }
    Write-Host ("Downloaded and extracted: " + $folderName)
    return $folderPath
}

# Self-update the test script: always pull the latest Test-KytyBuild.ps1
# from the kyty-test branch before running, so script fixes flow
# automatically and nobody has to replace files by hand. v7 FORCES the
# download (user's call, 2026-11-11): the script is downloaded to a temp
# file, validated (size and a known marker), and only then moved into
# place. If the download or validation fails, the watcher EXITS instead
# of running a test with a possibly stale script, and retries next tick.
$TestScriptUrl = "https://raw.githubusercontent.com/duy64/KytyPS5-nhl/kyty-test/Test-KytyBuild.ps1?cb=" + [DateTime]::UtcNow.Ticks
$TestScriptTmp = Join-Path $ScriptDir "Test-KytyBuild.new.ps1"
try {
    Invoke-WebRequest -Uri $TestScriptUrl -OutFile $TestScriptTmp -UseBasicParsing -TimeoutSec 60 -ErrorAction Stop
    $tmpBytes = Get-Item $TestScriptTmp -ErrorAction Stop
    $tmpText = Get-Content $TestScriptTmp -Raw -ErrorAction Stop
    if ($tmpBytes.Length -le 1000 -or $tmpText -notmatch 'Test-KytyBuild\.ps1') {
        throw "Downloaded test script failed validation (size or marker)."
    }
    Move-Item $TestScriptTmp $TestScript -Force -ErrorAction Stop
    Write-Host "Test script updated from GitHub."
} catch {
    Write-Host ("Test script download FAILED - will not run with a possibly stale script: " + $_.Exception.Message)
    if (Test-Path $TestScriptTmp) { Remove-Item $TestScriptTmp -Force -ErrorAction SilentlyContinue }
    exit 0
}

# v7: read the test script version ($TestScriptVersion = N in the script)
# so markers can be compared against it. A missing or unparsable version
# means 0, which re-tests everything once (safe default).
$script:TestScriptVersion = 0
try {
    $verLine = Select-String -Path $TestScript -Pattern '^\$TestScriptVersion\s*=\s*(\d+)' -ErrorAction Stop | Select-Object -First 1
    if ($verLine -and $verLine.Matches.Count -gt 0) { $script:TestScriptVersion = [int]$verLine.Matches[0].Groups[1].Value }
} catch { $script:TestScriptVersion = 0 }
Write-Host ("Test script version: " + $script:TestScriptVersion)

# Atlas capture, automatic (v5). The atlas-dump build (#139) holds the
# evidence for the versus corruption. A capture is "done" only when an
# atlas result folder holds a *_smp_*.bin file. Three manual runs missed
# versus, so the watcher now retries it by itself: if no bins exist
# anywhere (local captures or Drive) and fewer than 5 atlas attempts
# exist, run the capture on #139, then carry on with the normal pass.
# The atlas-request.txt flag still works as an override for any other tag.
function Test-AtlasDone {
    $marker = Join-Path $ScriptDir "atlas-done.txt"
    if (Test-Path $marker) { return $true }
    foreach ($root in @($ResultsRoot, $UploadDir)) {
        if ([string]::IsNullOrWhiteSpace($root)) { continue }
        if (-not (Test-Path $root)) { continue }
        $bins = @(Get-ChildItem -Path $root -Directory -Filter "*atlas*" -ErrorAction SilentlyContinue |
            ForEach-Object { Get-ChildItem -Path $_.FullName -Filter "*_smp_*.bin" -ErrorAction SilentlyContinue } |
            Where-Object { $_ })
        if ($bins.Count -gt 0) { return $true }
    }
    return $false
}
function Get-AtlasAttemptCount {
    $count = 0
    foreach ($root in @($ResultsRoot, $UploadDir)) {
        if ([string]::IsNullOrWhiteSpace($root)) { continue }
        if (-not (Test-Path $root)) { continue }
        $count += @(Get-ChildItem -Path $root -Directory -Filter "*atlas*" -ErrorAction SilentlyContinue).Count
    }
    return $count
}
function Invoke-AtlasCapture {
    param([string]$Tag)
    $atlasBuild = Get-BuildFolderForTag -Tag $Tag
    if ([string]::IsNullOrWhiteSpace($atlasBuild)) {
        Write-Host "Atlas capture: could not get the build folder."
        return $false
    }
    $stamp = Get-Date -Format "yyyy-MM-dd-HHmm"
    $atlasOut = Join-Path $ResultsRoot ("KyTyTest-" + (Split-Path $atlasBuild -Leaf) + "-atlas-" + $stamp)
    Set-Content -Path $LockFile -Value $PID -Encoding ASCII
    try {
        Write-Host ("Running atlas capture for: " + $atlasBuild)
        $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $TestScript + '" -BuildDir "' + $atlasBuild + '" -OutDir "' + $atlasOut + '" -AtlasCapture'
        $p = Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Wait -PassThru -WindowStyle Normal
        Write-Host ("Atlas test exit code: " + $p.ExitCode)
        if ((-not [string]::IsNullOrWhiteSpace($UploadDir)) -and (Test-Path $atlasOut)) {
            if (-not (Test-Path $UploadDir)) { New-Item -ItemType Directory -Force -Path $UploadDir | Out-Null }
            $dest = Join-Path $UploadDir (Split-Path $atlasOut -Leaf)
            Copy-Item $atlasOut $dest -Recurse -Force
            Write-Host ("Atlas results copied to: " + $dest)
            # v6 cleanup: the Drive copy is the record now. Delete the
            # local result folder so Downloads does not fill up, but only
            # when the copy actually landed and holds sampled bins.
            $copiedBins = @(Get-ChildItem -Path $dest -Filter "*_smp_*.bin" -ErrorAction SilentlyContinue)
            if ($copiedBins.Count -gt 0) {
                Set-Content -Path (Join-Path $ScriptDir "atlas-done.txt") -Value (Get-Date -Format "yyyy-MM-dd HH:mm") -Encoding ASCII -ErrorAction SilentlyContinue
                Write-Host "Atlas capture done (sampled bins present). Marker written."
                Remove-Item $atlasOut -Recurse -Force -ErrorAction SilentlyContinue
                Write-Host "Local atlas result folder deleted after Drive copy."
            }
        }
    }
    finally {
        if (Test-Path $LockFile) { Remove-Item $LockFile -Force }
    }
    return $true
}
$AtlasBuildTag = "KytyPS5-2026-11-10-4b197fb-f2dump"
$atlasFlag = Join-Path $ScriptDir "atlas-request.txt"
if (Test-Path $atlasFlag) {
    $atlasTag = ""
    try { $atlasTag = ((Get-Content $atlasFlag -ErrorAction Stop | Select-Object -First 1).Trim()) } catch { }
    if ([string]::IsNullOrWhiteSpace($atlasTag)) {
        Write-Host "Atlas request flag is empty; deleting it."
        Remove-Item $atlasFlag -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host ("Atlas request (flag file) for: " + $atlasTag)
        [void](Invoke-AtlasCapture -Tag $atlasTag)
        Remove-Item $atlasFlag -Force -ErrorAction SilentlyContinue
        Write-Host "Atlas request done; flag file deleted. Continuing to the normal build pass."
    }
} elseif (-not (Test-AtlasDone)) {
    $attempts = Get-AtlasAttemptCount
    if ($attempts -ge 5) {
        Write-Host ("Atlas capture: " + $attempts + " attempts already, none with sampled bins. Not retrying automatically.")
    } else {
        Write-Host ("Atlas capture: no sampled bins yet (" + $attempts + " attempts). Auto-running on " + $AtlasBuildTag + ".")
        [void](Invoke-AtlasCapture -Tag $AtlasBuildTag)
    }
} else {
    Write-Host "Atlas capture done (sampled bins exist). Skipping."
}

# Auto-download: fetch the newest test release from GitHub if we do not
# have it yet, so the loop is fully automatic (no manual download step).
# Only downloads releases from duy64/KytyPS5-nhl. If the API is unreachable
# or rate-limited, this is skipped silently and the watcher just tests
# whatever build folders are already on disk.
try {
    $releases = Invoke-RestMethod -Uri "https://api.github.com/repos/duy64/KytyPS5-nhl/releases?per_page=10" -UseBasicParsing -TimeoutSec 30 -ErrorAction Stop
    # The API does not guarantee order, so sort by created_at (newest first).
    # Accept both tag formats: KytyPS5-*-f2dump (older builds) and
    # KytyPS5-YYYY-MM-DD-<sha> (newer builds whose build.yml drops the
    # -f2dump suffix, e.g. #135 8bd6ff9). Exclude -main releases (daily play
    # builds, not test builds).
    $latestTag = $null
    $f2 = @($releases | Where-Object { $_.tag_name -like "KytyPS5-*" -and $_.tag_name -notlike "KytyPS5-*-main" } | Sort-Object { [datetime]$_.created_at } -Descending)
    if ($f2.Count -gt 0) { $latestTag = $f2[0].tag_name }
    if ($latestTag) {
        $folderName = $latestTag + "-Windows-x64"
        $zipName = $folderName + ".zip"
        $folderPath = Join-Path $BuildsRoot $folderName
        $zipPath = Join-Path $BuildsRoot $zipName
        if (-not (Test-Path $folderPath)) {
            $dlUrl = "https://github.com/duy64/KytyPS5-nhl/releases/download/" + $latestTag + "/" + $zipName
            Write-Host ("New release on GitHub: " + $latestTag + " - downloading.")
            Invoke-WebRequest -Uri $dlUrl -OutFile $zipPath -UseBasicParsing -TimeoutSec 600 -ErrorAction Stop
            $zipInfo = Get-Item $zipPath -ErrorAction SilentlyContinue
            if (-not $zipInfo -or $zipInfo.Length -lt 1000000) {
                Write-Host ("Download failed or too small (" + $zipInfo.Length + " bytes). Deleting bad zip.")
                Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
            } else {
                Write-Host ("Extracting: " + $zipName)
                # Extract to a temp dir first, then normalize into the build
                # folder. The zip holds loose files at its root (no top-level
                # folder), so extracting straight to D:\Downloads spills
                # kyty_emulator.exe etc. into Downloads itself.
                $tmpDir = Join-Path $BuildsRoot ("_extract_" + $folderName)
                if (Test-Path $tmpDir) { Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue }
                Expand-Archive -Path $zipPath -DestinationPath $tmpDir -Force -ErrorAction Stop
                if (-not (Test-Path $folderPath)) { New-Item -ItemType Directory -Force -Path $folderPath | Out-Null }
                $top = @(Get-ChildItem -Path $tmpDir -Force -ErrorAction SilentlyContinue)
                $singleDir = $null
                if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $singleDir = $top[0] }
                if ($singleDir) {
                    Get-ChildItem -Path $singleDir.FullName -Force | ForEach-Object { Move-Item $_.FullName (Join-Path $folderPath $_.Name) -Force }
                } else {
                    Get-ChildItem -Path $tmpDir -Force | ForEach-Object { Move-Item $_.FullName (Join-Path $folderPath $_.Name) -Force }
                }
                Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
                Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
                Write-Host ("Downloaded and extracted: " + $folderName)
            }
        }
    }
} catch {
    Write-Host ("Auto-download skipped (GitHub unreachable or rate-limited): " + $_.Exception.Message)
}

# v8: a Drive copy is only complete when it holds the full key set.
# The old rule (transcript.txt alone, or 04-versus.png alone) let a
# partial copy look complete, and the local folder was deleted while
# the nav shots, the ice shot, and the frame.txt files never made it
# to Drive (1216 and 1516 runs). The frame.txt files ride along with
# the shots, so the shot files are the gate.
function Test-DriveCopyComplete {
    param([string]$DriveFolder)
    foreach ($f in @("transcript.txt", "04-versus.png", "05-ice.png", "07-pause-menu.png")) {
        if (-not (Test-Path (Join-Path $DriveFolder $f))) { return $false }
    }
    return $true
}

# Backfill: upload any Downloads KyTyTest result folders that never made it
# to Drive (e.g. a manual run that did not pass -UploadDir). Runs whenever
# the watcher passes, so a missed upload is picked up on the next pass
# without anyone re-running the test. v6 also deletes the local folder
# after a successful backfill, so Downloads does not fill up with copies
# that already live on Drive. A folder is only deleted when its Drive
# copy exists and holds the versus shot (conclusive) or the transcript
# (so a stalled run's evidence is not lost).
function Sync-ResultsToDrive {
    if ([string]::IsNullOrWhiteSpace($UploadDir)) { return }
    $results = Get-ChildItem -Path $ResultsRoot -Directory -Filter "KyTyTest-*" -ErrorAction SilentlyContinue
    foreach ($r in $results) {
        $dest = Join-Path $UploadDir $r.Name
        if (-not (Test-Path $dest)) {
            if (-not (Test-Path $UploadDir)) { New-Item -ItemType Directory -Force -Path $UploadDir | Out-Null }
            Copy-Item $r.FullName $dest -Recurse -Force
            Write-Host ("Backfilled results to Drive: " + $r.Name)
        }
        # v8: only delete local when the Drive copy holds the full key
        # set. A partial copy (transcript only) keeps its local folder
        # so the next pass retries the upload instead of losing the
        # nav shots, the ice shot, and the frame.txt files.
        if (Test-DriveCopyComplete -DriveFolder $dest) {
            Remove-Item $r.FullName -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host ("Local result folder deleted after Drive copy: " + $r.Name)
        } else {
            Write-Host ("Drive copy incomplete for " + $r.Name + " - keeping local folder for the next pass.")
        }
    }
}

# v6 cleanup at the start of each pass: remove leftover _extract_* temp
# folders from a previous interrupted download/extract, so a stale temp
# dir never blocks or confuses the next pass.
Get-ChildItem -Path $BuildsRoot -Directory -Filter "_extract_*" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host ("Removing leftover extract temp folder: " + $_.Name)
    Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
}

# Pick the newest build that has no conclusive result yet. v6 sorts by
# the release date in the tag (newest first), not by folder LastWriteTime,
# so a just re-downloaded older build does not jump ahead of a newer one.
$builds = Get-ChildItem -Path $BuildsRoot -Directory -Filter "KytyPS5-*-Windows-x64" -ErrorAction SilentlyContinue |
          Where-Object { Test-Path (Join-Path $_.FullName "kyty_emulator.exe") } |
          Sort-Object { $d = Get-BuildDate $_.Name; if ($d) { $d } else { $_.LastWriteTime } } -Descending
$target = $null
foreach ($b in $builds) {
    if (-not (Test-HasConclusiveResult $b.Name)) { $target = $b; break }
}
if (-not $target) { Write-Host "No new build to test."; Sync-ResultsToDrive; exit 0 }

Write-Host ("New build found: " + $target.Name)
$buildDir = $target.FullName
$stamp = Get-Date -Format "yyyy-MM-dd-HHmm"
$outDir = Join-Path $ResultsRoot ("KyTyTest-" + $target.Name + "-" + $stamp)

Set-Content -Path $LockFile -Value $PID -Encoding ASCII
try {
    Write-Host ("Running test for: " + $buildDir)
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $TestScript + '" -BuildDir "' + $buildDir + '" -OutDir "' + $outDir + '"'
    $p = Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Wait -PassThru -WindowStyle Normal
    Write-Host ("Test exit code: " + $p.ExitCode)
    # Whether this build is "done" is decided by its result folder
    # (04-versus.png present = reached the game). A stalled run leaves only
    # nav shots, so it is re-tested next pass.
    if ((-not [string]::IsNullOrWhiteSpace($UploadDir)) -and (Test-Path $outDir)) {
        if (-not (Test-Path $UploadDir)) { New-Item -ItemType Directory -Force -Path $UploadDir | Out-Null }
        $dest = Join-Path $UploadDir (Split-Path $outDir -Leaf)
        Copy-Item $outDir $dest -Recurse -Force
        Write-Host ("Results copied to: " + $dest)
        # v8: give the Drive client 3 minutes to upload before judging
        # the copy, and only mark tested + delete local when the Drive
        # copy holds the full key set. The old rule (04-versus.png
        # alone, checked right after the copy) let a partial upload
        # look complete. A stalled run (no 04-versus.png) keeps its
        # local folder so the next pass can re-test it.
        Write-Host "Waiting 3 min for the Drive client to upload before checking the copy."
        Start-Sleep -Seconds 180
        if (Test-DriveCopyComplete -DriveFolder $dest) {
            Set-TestedMarker -BuildName $target.Name
            Write-Host "Build marked tested."
            Remove-Item $outDir -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host "Local result folder deleted after Drive copy."
        } else {
            Write-Host "Drive copy incomplete - keeping local folder; the next pass backfills it."
        }
    } elseif (Test-Path (Join-Path $outDir "04-versus.png")) {
        # No Drive upload dir configured, but the run was conclusive.
        Set-TestedMarker -BuildName $target.Name
        Write-Host "Build marked tested (no Drive upload configured)."
    }
    Sync-ResultsToDrive
}
finally {
    if (Test-Path $LockFile) { Remove-Item $LockFile -Force }
}
# Delete old build folders to save disk space. Any build can be
# re-downloaded from GitHub if needed. Keep the build just tested and any
# build without a conclusive result; delete older builds that already
# reached the game.
Get-ChildItem -Path $BuildsRoot -Directory -Filter "KytyPS5-*-Windows-x64" -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.Name -ne $target.Name -and (Test-HasConclusiveResult $_.Name)) {
        Write-Host ("Deleting old build: " + $_.Name)
        Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
}
Write-Host "Done."
