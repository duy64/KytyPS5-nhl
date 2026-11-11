# Test-KytyBuild.ps1 - v29 (2026-11-11)
# Drives NHL 27 in the KyTy emulator from launch to the versus screen and
# through a full game of dumb gameplay, using OCR-guided navigation and the
# emulator's scripted input file. No window-focus tricks.
#
# v29 handles the Be A Pro X-Factors screen: the v28 run sat on it
# because the classifier returned Unknown (which just waits) and the
# loop never backed out. XFactor is now a recognized kind; the nav loop
# presses Circle to return to the hub.
#
# v28 downscales screenshots to 1080p before OCR. Tesseract on a full
# 3840x2160 image is slow and CPU-heavy, and it starves the emulator:
# in the 1316 run the FPS collapsed from 60 to 5-6 during Versus
# navigation, when the script runs OCR on every nav shot, and never
# recovered. Manual play never hits this because no OCR runs at all.
# The UI text the classifier needs is large and reads fine at 1080p.
#
# v27 brings the emulator window to the front before every screenshot
# (Invoke-BringEmulatorToFront, AttachThreadInput trick so a background
# scheduled task can steal focus). The screen capture grabs the foreground
# window, so if the browser or anything else is covering the game, the
# nav shots capture that instead and OCR reads the wrong screen - the
# Nov 9 background-task failure. The emulator must be in front for the
# screenshots, not just running.
#
# v26 waits 10 s after the display wake before launching, and logs the
# pre-launch machine state (workstation locked? idle time since last
# input). v25's wake ran on the 10:16 run and the emulator still died
# about 1 s after launch in vkGetPhysicalDeviceSurfaceCapabilitiesKHR
# (ErrorUnknown -13), with the assert site confirmed in
# emulator-stdout.log: src/graphics/host_gpu/vulkanCommon.cpp:123.
# The wake request starts the wake, but the display path may not be up
# when the first surface query lands, so the launch now waits for it.
# The state log tells the next boot exit apart: a locked workstation
# (logonui running) leaves no usable surface at all and needs the user,
# not a longer wait.
#
# v25 captures the emulator's stdout/stderr to emulator-stdout.log and
# emulator-stderr.log in the result folder, and wakes the display before
# launch. The 09:46 run (#146) exited during boot with code 321, which
# is EXIT_HALT (Common::DbgExit(321), src/common/assert.h) - a fatal
# assert, not an access violation. The fatal report is written by
# Log::WriteFatal, which ALWAYS prints to stdout, but the script never
# redirected the emulator's stdout, so the report was lost and
# last-run.log was never rewritten (STALE). Redirecting stdout/stderr
# with async readers captures the fatal report (file:line + host stack)
# on the next boot exit. The display wake is ported from the Nov 9
# local commit that never reached this branch: the 2026-11-09 04:59
# auto-test of #135 crashed at launch in
# vkGetPhysicalDeviceSurfaceCapabilitiesKHR (ErrorUnknown -13) with the
# monitor off overnight, and the same build runs fine with the monitor
# on. Seven consecutive morning boot exits on #146 (08:16-09:46) fit
# the same pattern - the display is off/asleep when the watcher
# launches. ES_DISPLAY_REQUIRED wakes it for the test run.
#
# v24 reports the emulator exit code and checks last-run.log freshness on
# a boot exit, instead of blindly copying the log. The 09:16 run copied a
# 28 MB log that was still the 22:58 session (167,159 lines, t=5999s) -
# the emulator does not rewrite last-run.log when it exits during boot,
# so the copy proved nothing and five consecutive boot exits on #146
# (08:16, 08:31, 08:46, 09:01, 09:16) arrived with no usable evidence.
# A stale log is now labelled STALE and not copied; the exit code
# (-1073741819 = 0xC0000005 access violation, 0 = clean exit) is the one
# fact a boot exit always leaves behind.
#
# v23 copies last-run.log into the result folder on a boot exit, so the
# v22 fixes the frame-dump capture so the auto-run can diagnose the white
# lines on the ice (user-confirmed Nov 11): the emulator writes each dump
# to a NEW frame_N folder, but Save-FrameDumpShot only watched frame_0,
# so every dump after the first was missed and gameplay shots fell back
# to screen captures. It now watches the newest frame_* folder (re-checked
# each pass) and copies frame.txt into the result folder so the monitor
# can analyze which shader draws the ice. Deadline raised 60s to 90s
# (at 1-2 fps the 30-flip trigger check alone takes 15-30s).
#
# v21 fixes v20's fatal hoisting bug: v20 defined Test-FileExistsQuiet
# below the startup code that calls it, and PowerShell does not hoist
# function definitions, so the 0646 run died instantly ("not recognized").
# The helper is now defined right after the param block, before any
# top-level code calls it. Version bumped to 21 so the watcher re-tests
# builds under the fixed script.
#
# v20 adds $TestScriptVersion so the watcher can re-test a build when the
# script changes: a build marked tested under an older script version gets
# re-run automatically, so a script fix (like v19's) re-tests builds
# instead of the marker blocking it forever.
#
# v19 fixes the 2026-11-10 2342 run (#146, 36fd0b1), which died at cycle
# 528 (~49 min, exit code 1) with Test-Path "Access is denied" on
# kyty-input.txt: the emulator holds the input file open while reading
# it, and Test-Path throws a terminating error under ErrorActionPreference
# Stop. New Test-FileExistsQuiet helper wraps the check in try/catch so
# a locked input file never kills the run; used for all three
# Test-Path $InputFile calls.
#
# v18 fixes the 2026-11-10 2258 run (#146, 36fd0b1), which never reached
# the ice even though the game was fine - the versus screen was clean
# (both team logos render) and the game advanced into the pre-game tunnel
# cinematic on its own. Three script gaps burned the 60-shot budget:
#  - The Be A Pro hub was read as Versus: the hub preview headline says
#    "Game Preview: ... Face-off Against ...", and the bare "face-off"
#    Versus marker fired. "play next game" now classifies Hub BEFORE
#    the Versus check (it is distinct from the Play Now "play now" the
#    v9 Versus-first ordering protects).
#  - The Be A Pro versus splash was read as Unknown: "VERSUS" is written
#    vertically so OCR misses it, and the only text OCR reads is
#    "AWAY | 53-1-0". "away" plus a W-L-OTL record now classifies
#    Versus (OCR reads 0 as O, so the record pattern allows O).
#  - Loading / cinematic pace burned the budget: the frame froze at 8598
#    (fps 0) for 11 shots, then crept to 8641 through the tunnel at
#    0-2 fps. Shots taken at loading pace (same frame as the last shot,
#    or fps 2 or less) no longer count toward the 60-shot budget, and
#    Unknown screens at that pace wait 10 s and press nothing. A 25
#    minute nav deadline and a 30-shot frozen streak are the backstops,
#    so a genuinely stuck game still fails instead of waiting forever.
#
# v17 efficiency: the gameplay OCR check ran every 6 cycles (about every
# 30 seconds), and each check runs Tesseract, which costs CPU and slows
# the game a little. The check only needs to catch Pause, a return to a
# menu (game over), or a long Unknown streak - none of which need a
# 30-second response. v17 runs it every 12 cycles (about every minute),
# halving the OCR overhead. The flash-watch (every 5 cycles) and the
# 10-minute frame-dump data shots are unchanged.
#
# v16 adds a flash-watch to the gameplay phase (user's call, 2026-11-10):
# the 10-minute frame-dump data shots can miss brief/transient glitches
# (a flash that lasts a frame or two). During STEP 4 a fast screen
# capture is now taken every 5 cycles (about 30 seconds) and saved as
# 12-flash-<n>.png, so the monitor can scan for flashes, garbled frames
# and other brief glitches the 10-minute shots would miss. Screen
# captures are cheap (no frame dump), so this is affordable: about 180
# shots over a 90-minute game. The 10-minute frame-dump data shots are
# unchanged.
#
# v15 fixes the 2026-11-10 1736 run (#139 atlas, 4b197fb), which never
# tested the versus screen or the pause menu even though the transcript
# claims "Gameplay shots complete: 04-versus, ... 07 pause menu":
#  - After PLAY NEXT GAME the game shows a "PLEASE CHOOSE A GAME
#    EXPERIENCE" dialog (CUSTOM / QUICK / MEDIUM / REAL-TIME). v14 had
#    no stop for it, and Get-ScreenKind read it as Ice because the
#    dialog says "5 MINUTE PERIODS" and Ice matched bare "period".
#    The roadmap is now Title -> Hub -> GameExperience -> Versus ->
#    Ice, and the GameExperience stop confirms the highlighted choice
#    (CUSTOM, the default - the same choice the 1736 run confirmed by
#    accident, which reached versus and the ice fine). Do NOT move the
#    highlight: changing the preset is the user's call.
#  - Ice no longer matches bare "period"/"shots"/"goals"/"hits": it
#    needs real HUD text ("sog", "power play", "penalty kill") or a
#    game clock with a period ("13:54" + "1ST"). The dialog says
#    "PERIODS" and the goal-summary screen says "GOAL SUMMARY", so
#    both used to read as Ice.
#  - The STEP 3 completion line only claims the versus shot when the
#    Versus stop was actually reached, and the pause-menu shot is
#    OCR-checked: if Options did not open a pause menu, the transcript
#    says so instead of calling the shot a pause menu.
#
# v13 backs up the real save before every run (user's call). The test
# already plays on a copy: it copies the run-folder save into the build
# folder's _SaveData, and the game reads/writes that copy, so the real
# save in %LOCALAPPDATA%\KyTyPS5\run\_SaveData is not touched. v13 adds
# a second copy - the run-folder save into save-backup in the result
# folder - so every run leaves a safe copy that rides to Drive with the
# results. If anything ever goes wrong, restore that folder to
# %LOCALAPPDATA%\KyTyPS5\run\_SaveData.
#
# v14 stops the nav loop from giving up while the Be A Pro hub is still
# loading. The 2026-11-10 1651 (#142 delta) and 1706 (#139 atlas) runs
# both died at the Versus stop after 8 tries without ever seeing versus:
# the hub load runs at 2-12 fps and takes ~90 s, and every Unknown
# loading-screen wait consumed a try, so the cap fired just as the hub
# appeared (1706 shot 11 shows the hub fading in, fps back to 53).
# A try is now consumed only by a RECOGNIZED screen - a loading screen
# waits without consuming one. The 60-shot global cap is still the
# backstop for a genuinely stuck load.
#
# v10 fixes the stuck-versus run (2026-11-10 16:28, #142 delta, v9). The
# emulator sat at the Play Now versus screen at a healthy 47 fps while the
# script did nothing - the game was fine, the script was looping. Two
# causes, both from the 1536 atlas run (245 nav shots to reach versus):
#  - The Hub action used left-stick Down ("s") to move the highlight, but
#    the menu may only answer the D-pad. The highlight never moved, Cross
#    kept opening BE A PRO CAREER, and the roadmap ping-ponged Hub <->
#    Versus. Menu navigation now uses D-pad Down ("Down").
#  - The roadmap jump resets the per-stop try counter, so a stuck game
#    ping-pongs forever and never fails. A global cap of 60 nav shots now
#    ends the loop with the stop it stuck on, instead of running all day.
#
# What is different from the v2 script (and why v2 never got past the title):
#  - The PSN sign-in dialog is handled FIRST, explicitly, before any title
#    input. v2 only tried to dismiss it after 3 Unknown reads, by which time
#    Cross presses were already being eaten by the dialog. The 2026-11-09
#    1204 run proved the dialog sits on top of the title screen ("Press any
#    button" visible behind it) and the test never got past it.
#  - Launch matches manual play: 3840x2160. 720p was tried and does not
#    stick - the emulator/game still comes up at 3840x2160 even when the
#    script passes 1280x720, so 4K is what the test actually runs at.
#    --skip-notice-screen, exactly as the launcher passes it.
#  - 90 s boot wait (v2 cut it to 45 s; the game needs the full time to load
#    the title screen and the sign-in dialog before input means anything).
#  - Everything else kept is proven: the frame-dump trigger file in BOTH the
#    build folder and the run folder, KYTY_INPUT_FILE env, screen-capture
#    fallback, and the fail-fast checks that stop the run if the emulator
#    exits instead of screenshotting the desktop for 25 minutes.
#
# Usage (defaults suit the watcher; the bat file runs it with no args):
#   powershell -NoProfile -ExecutionPolicy Bypass -File Test-KytyBuild.ps1
#   [-GamePath "D:\Downloads\ps5 games\PPSA34063-app0"]
#   [-OutDir "D:\Downloads\captures\KyTyTest-..."] [-ReleaseTag "..."]
#   [-BootWaitSec 90] [-ScreenWidth 3840] [-ScreenHeight 2160]
#   [-KeepRunning] [-SkipGameplay] [-NoOcr] [-GameplayMinutes 90]
#   -GameplayMinutes caps the STEP 4 dumb-gameplay phase (default 90, a
#   full game at real-time pace with stoppages; pass 30 for a shorter run).
#   The phase ends early if the game returns to a menu.
#   -AtlasCapture runs a short versus-only capture for the atlas-dump
#   build: sets KYTY_DBG_FRAME_DUMP_SAMPLED=1, stops at the versus screen,
#   dumps the sampled BC textures (*_smp_*.bin) into the result folder,
#   then stops. No gameplay. Pass -BuildDir for the atlas-dump build.

param(
    [string]$GamePath = "D:\Downloads\ps5 games\PPSA34063-app0",
    [string]$EmulatorExe = "",
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [string]$ReleaseTag = "",
    [int]$BootWaitSec = 90,
    [int]$LoadWaitSec = 60,
    [int]$ScreenWidth = 3840,
    [int]$ScreenHeight = 2160,
    [switch]$KeepRunning,
    [switch]$SkipGameplay,
    [switch]$NoOcr,
    [switch]$AtlasCapture,
    [int]$GameplayMinutes = 90
)

$ErrorActionPreference = "Stop"

# The watcher reads this number to decide whether a build needs re-testing:
# a build marked tested with an older script version gets re-run when the
# script version increases, so a script fix automatically re-tests builds.
$TestScriptVersion = 29

# Test-Path throws a terminating error (ErrorActionPreference is Stop) when a
# file is locked by another process ("Access is denied"). The emulator holds
# kyty-input.txt open while reading it, so a plain Test-Path on it can kill
# the whole run. That is what ended the 2342 run at cycle 528. This helper
# never throws: locked or unreadable means "not there" for our purposes.
# Defined here, before any top-level code calls it: PowerShell does not
# hoist function definitions, so a helper used by startup code must be
# defined above that code (v20 defined it below the startup call and the
# 0646 run died instantly with "not recognized").
function Test-FileExistsQuiet {
    param([string]$Path)
    try { return (Test-Path $Path) } catch { return $false }
}

# Cleanup: frame dumps (D:\Downloads\captures\frame_*) from prior runs pile
# up - the emulator writes a new frame_N folder per dump and nothing ever
# removes them. Clear the old ones at the start of each run so only this
# run's dumps remain (this run's dumps stay until the next run starts).
$captureDir = "D:\Downloads\captures"
if (Test-Path $captureDir) {
    Get-ChildItem -Path $captureDir -Directory -Filter "frame_*" -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem -Path $captureDir -Directory -Filter "KyTyTest-*" -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "captures: cleared old frame dumps and shot folders"
}

# Build: auto-detects the newest KyTyPS5 build folder under D:\Downloads
# (D:\Downloads\<build-name>, e.g. KytyPS5-2026-11-09-8bd6ff9-Windows-x64).
# The watcher passes -BuildDir for a specific build; a manual bat run leaves
# it blank, so the newest build is used. The script itself lives in
# D:\Downloads\NHL runtest\, which holds no emulator - never derive the
# build folder from the script's own path.
function Find-LatestBuild {
    $dl = "D:\Downloads"
    if (-not (Test-Path $dl)) { return "" }
    $best = ""; $bestTime = [DateTime]::MinValue
    foreach ($dir in Get-ChildItem -Path $dl -Directory -Filter "KytyPS5-*-Windows-x64" -ErrorAction SilentlyContinue) {
        $exe = Join-Path $dir.FullName "kyty_emulator.exe"
        if ((Test-Path $exe) -and ($dir.LastWriteTime -gt $bestTime)) {
            $bestTime = $dir.LastWriteTime; $best = $dir.FullName
        }
    }
    return $best
}

# Resolve the emulator exe and build folder.
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Find-LatestBuild }
if ([string]::IsNullOrWhiteSpace($BuildDir)) { throw "No KyTy build found in D:\Downloads (check -BuildDir)" }
if (-not $EmulatorExe) { $EmulatorExe = Join-Path $BuildDir "kyty_emulator.exe" }
if (-not (Test-Path $EmulatorExe)) { throw ("Emulator not found: " + $EmulatorExe + " (check -BuildDir)") }
if (-not (Test-Path $GamePath)) { throw ("Game path not found: " + $GamePath) }
Write-Host ("build: " + $BuildDir)

# Result folder. The watcher passes -OutDir; a manual run gets a stamped
# folder inside D:\Downloads\captures, next to the frame dumps, so all
# test output lives in one place.
if (-not $OutDir) {
    $stamp = Get-Date -Format "yyyy-MM-dd-HHmm"
    $tag = $ReleaseTag
    if (-not $tag) { $tag = Split-Path $BuildDir -Leaf }
    $OutDir = Join-Path "D:\Downloads\captures" ("KyTyTest-" + $tag + "-" + $stamp)
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$TranscriptPath = Join-Path $OutDir "transcript.txt"
Start-Transcript -Path $TranscriptPath -Force | Out-Null
Write-Host ("out:   " + $OutDir)
Write-Host ("game:  " + $GamePath)

# Tesseract OCR (for reading the screen). Found once, reused everywhere.
$script:TesseractExe = $null
foreach ($cand in @("C:\Program Files\Tesseract-OCR\tesseract.exe",
                    "C:\Program Files (x86)\Tesseract-OCR\tesseract.exe")) {
    if (Test-Path $cand) { $script:TesseractExe = $cand; break }
}
if ($script:TesseractExe) { Write-Host ("ocr:   " + $script:TesseractExe) }
else { Write-Warning "Tesseract not found; navigation runs blind (Cross/Options only)." }

# Scripted-input file the emulator watches (KYTY_INPUT_FILE). The emulator
# reads it, then DELETES it, and re-polls every 100 ms. Lines are
# "<SDL keyname> [hold ms]" or "sleep <ms>".
# Key map (emulator default keyboard): j=Cross, l=Circle, i=Triangle,
# k=Square, arrows=D-pad, w/a/s/d=left stick, f/h/t/g=right stick,
# Return=Options (Start).
$InputFile = Join-Path $OutDir "kyty-input.txt"
if (Test-FileExistsQuiet $InputFile) { Remove-Item $InputFile -Force }
$env:KYTY_INPUT_FILE = $InputFile

# Copy the saved game from the launcher's run folder into the build folder
# before launch, so the game loads the existing Be A Pro save and reaches the
# hub instead of the new-game flow. SAVE_DATA_DIR is relative "_SaveData".
$runSave = Join-Path $env:LOCALAPPDATA "KytyPS5\run\_SaveData"
$buildSave = Join-Path $BuildDir "_SaveData"
if (Test-Path $runSave) {
    try {
        if (-not (Test-Path $buildSave)) { New-Item -ItemType Directory -Force -Path $buildSave | Out-Null }
        Copy-Item -Path (Join-Path $runSave "*") -Destination $buildSave -Recurse -Force -ErrorAction Stop
        Write-Host "saves: copied from run folder to build folder"
    } catch {
        Write-Warning ("saves: copy failed, continuing without it: " + $_.Exception.Message)
    }
} else {
    Write-Host "saves: no run-folder saves found, continuing without them"
}

# Back up the real save into the result folder (v13). The test plays on
# the build-folder copy above, so the run-folder save is not touched -
# but keep a safe copy in the result folder anyway, so every run leaves
# a backup that rides to Drive with the results. Restore it to
# %LOCALAPPDATA%\KyTyPS5\run\_SaveData if anything ever goes wrong.
if (Test-Path $runSave) {
    try {
        $backupDir = Join-Path $OutDir "save-backup"
        New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
        Copy-Item -Path (Join-Path $runSave "*") -Destination $backupDir -Recurse -Force -ErrorAction Stop
        Write-Host "save backup: copied run-folder saves to save-backup in the result folder"
    } catch {
        Write-Warning ("save backup: copy failed, continuing: " + $_.Exception.Message)
    }
}

# Write input lines to the file, then BLOCK until the emulator consumes it
# (deletes it), up to 30 s. If it is never consumed, the emulator did not get
# KYTY_INPUT_FILE, and pressing on would be pointless.
function Invoke-InputStage {
    param([string[]]$Lines, [int]$ExtraWaitSec = 2)
    $text = ($Lines -join "`r`n") + "`r`n"
    Write-Host ("input stage: " + ($Lines -join " | "))
    Set-Content -Path $InputFile -Value $text -Encoding ASCII -NoNewline
    $deadline = (Get-Date).AddSeconds(30)
    while ((Test-FileExistsQuiet $InputFile) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 200 }
    if (Test-FileExistsQuiet $InputFile) {
        Write-Warning "Input file was not consumed; the emulator may not have KYTY_INPUT_FILE set."
        Remove-Item $InputFile -Force -ErrorAction SilentlyContinue
    }
    if ($ExtraWaitSec -gt 0) { Start-Sleep -Seconds $ExtraWaitSec }
}

# Diagnose a frame-dump timeout from the emulator log, so the run says WHY
# the dump never fired instead of silently falling back to a screen capture.
# Writes to the host and appends to frame-dump-diagnosis.txt in the result
# folder. The log may be stale (from an earlier run) if this run never wrote
# one, so the messages say so.
function Write-FrameDumpDiagnosis {
    $diag = @()
    if (-not (Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue)) {
        $diag += "Frame dump diagnosis: the emulator process (kyty_emulator) is no longer running, so this trigger cannot fire. The emulator exited before this shot - check last-run.log for why."
    }
    $logPath = Join-Path $env:LOCALAPPDATA "KytyPS5\logs\last-run.log"
    if (-not (Test-Path $logPath)) {
        $diag += ("Frame dump diagnosis: no last-run.log found at " + $logPath + ", so the dump state cannot be checked.")
    }
    else {
        $logText = Get-Content $logPath -Raw -ErrorAction SilentlyContinue
        if ([string]::IsNullOrWhiteSpace($logText)) {
            $diag += "Frame dump diagnosis: last-run.log is empty; no dump evidence."
        }
        else {
            $dumpLines = @($logText -split "`r?`n" | Where-Object { $_ -match "NHL27FRAMEDUMP" })
            if ($dumpLines.Count -eq 0) {
                $diag += "Frame dump diagnosis: last-run.log has no NHL27FRAMEDUMP line at all, so the dump never fired in that run (KYTY_DBG_FRAME_DUMP likely unset, or the trigger was never consumed). Note: this log may be stale, from an earlier run."
            }
            else {
                $bad = @($dumpLines | Where-Object { $_ -match "cannot create" })
                if ($bad.Count -gt 0) {
                    $diag += ("Frame dump diagnosis: the log shows the dump tried and failed to create its output folder: " + ($bad[0].Trim()) + " (the dump dir env var likely did not reach the emulator, so it fell back to its built-in dir).")
                }
                else {
                    $diag += ("Frame dump diagnosis: the log shows the dump fired before (" + $dumpLines.Count + " NHL27FRAMEDUMP lines) but this trigger produced no new frame; the trigger file may not be in the emulator working directory.")
                }
            }
        }
    }
    foreach ($line in $diag) { Write-Warning $line }
    if ($OutDir -and (Test-Path $OutDir)) {
        try {
            $diagFile = Join-Path $OutDir "frame-dump-diagnosis.txt"
            $stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
            ($diag | ForEach-Object { $stamp + "  " + $_ }) | Add-Content -Path $diagFile -Encoding ASCII -ErrorAction Stop
        } catch {
            Write-Warning ("Could not write frame-dump-diagnosis.txt: " + $_.Exception.Message)
        }
    }
}

# Take one frame-dump shot and save it as $ShotName in the result folder.
# Returns the saved path, or $null on failure (caller falls back to a screen
# capture). The emulator checks a RELATIVE "framedump.trigger" in its own
# working directory every 30 flips and removes it when found, so the trigger
# is created in BOTH the build folder (the emulator CWD when the test
# launches it) and the run folder (where the launcher would look).
# NOTE: F2 via the scripted input file does NOT trigger a frame dump. F2 is
# only handled in the SDL window event loop for real keyboard presses;
# scripted input maps keys to gamepad controls instead. The trigger file is
# the only scripted frame-dump path.
function Save-FrameDumpShot {
    param([string]$ShotName)
    $dumpDir = $env:KYTY_DBG_FRAME_DUMP_DIR
    if (-not $dumpDir) { $dumpDir = "D:\Downloads\captures" }
    # v22: the emulator writes each dump to a NEW frame_N folder (frame_0,
    # frame_1, ...), so watching only frame_0 misses every dump after the
    # first. Find the newest frame_* folder at call time and watch that.
    $frameDir = $null
    $frameDirs = @(Get-ChildItem -Path $dumpDir -Directory -Filter "frame_*" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
    if ($frameDirs.Count -gt 0) { $frameDir = $frameDirs[0].FullName }
    if (-not $frameDir) { $frameDir = Join-Path $dumpDir "frame_0" }
    $before = @()
    if (Test-Path $frameDir) {
        $before = @(Get-ChildItem -Path $frameDir -Filter "*.png" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
    }
    $runTrig = Join-Path $env:LOCALAPPDATA "KytyPS5\run\framedump.trigger"
    New-Item -ItemType File -Force -Path $runTrig | Out-Null
    $buildTrig = Join-Path $BuildDir "framedump.trigger"
    New-Item -ItemType File -Force -Path $buildTrig | Out-Null
    $deadline = (Get-Date).AddSeconds(90)
    $found = $null
    $fallback = $null
    $foundDir = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        # Re-check for the newest frame_* folder each pass: the dump may
        # create a new folder (frame_1, frame_2, ...) after the trigger.
        $latest = @(Get-ChildItem -Path $dumpDir -Directory -Filter "frame_*" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
        if ($latest.Count -gt 0) { $frameDir = $latest[0].FullName }
        if (Test-Path $frameDir) {
            $now = @(Get-ChildItem -Path $frameDir -Filter "*.png" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
            foreach ($f in $now) {
                if ($before -notcontains $f.Name) {
                    # Prefer the _rt0_ color image: the first new PNG can be
                    # a depth image (6144x6144) or a storage image, and OCR
                    # on a depth map reads garbage. Keep the first new PNG
                    # as a fallback in case no rt0 appears.
                    if (-not $fallback) { $fallback = $f.FullName; $foundDir = $frameDir }
                    if ($f.Name -match "_rt0_") { $found = $f.FullName; $foundDir = $frameDir; break }
                }
            }
            if ($found) { break }
        }
        if (-not (Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue)) { break }
    }
    if (-not $found) { $found = $fallback }
    if (-not $found) { Write-FrameDumpDiagnosis; return $null }
    $dest = Join-Path $OutDir $ShotName
    Copy-Item -Path $found -Destination $dest -Force
    Write-Host ("shot (dump): " + $dest)
    # v22: copy frame.txt (the draw list) into the result folder so the
    # monitor can analyze which shader draws what (e.g. the white lines
    # on the ice) without needing the whole dump folder.
    if ($foundDir) {
        $frameTxt = Join-Path $foundDir "frame.txt"
        if (Test-Path $frameTxt) {
            $txtDest = Join-Path $OutDir ($ShotName -replace '\.png$', '-frame.txt')
            Copy-Item -Path $frameTxt -Destination $txtDest -Force -ErrorAction SilentlyContinue
        }
    }
    return $dest
}

# Wait until the frame-dump download is finished: no new PNG has appeared
# in frame_0 for 10 seconds, or 120 seconds have passed. A capture writes
# 300 images over minutes; a trigger fired while that download is still
# running is swallowed and produces no new capture (proven by the v6 atlas
# run, where 14 nav shots were all PNGs from the first capture). Call this
# before the one frame dump that matters (the versus capture).
function Wait-FrameDumpQuiescent {
    $dumpDir = $env:KYTY_DBG_FRAME_DUMP_DIR
    if (-not $dumpDir) { $dumpDir = "D:\Downloads\captures" }
    $frameDir = Join-Path $dumpDir "frame_0"
    $deadline = (Get-Date).AddSeconds(120)
    $lastCount = -1
    $quietSince = Get-Date
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        $count = 0
        if (Test-Path $frameDir) {
            $count = @(Get-ChildItem -Path $frameDir -Filter "*.png" -ErrorAction SilentlyContinue).Count
        }
        if ($count -ne $lastCount) {
            $lastCount = $count
            $quietSince = Get-Date
        } elseif (((Get-Date) - $quietSince).TotalSeconds -ge 10) {
            Write-Host ("frame dump quiescent: " + $count + " PNGs in frame_0, none new for 10 s.")
            return
        }
    }
    Write-Warning "Frame dump did not go quiescent in 120 s; proceeding anyway."
}

# Fallback: capture the foreground window to $ShotName. Only used when the
# frame dump fails. Warns that this grabs whatever is in front, which may
# not be the game.
function Save-ScreenShot {
    param([string]$ShotName)
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
    Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
    # v27: bring the emulator to the front first, or the capture grabs
    # whatever window is covering the game.
    Invoke-BringEmulatorToFront | Out-Null
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $g.Dispose()
    $dest = Join-Path $OutDir $ShotName
    $bmp.Save($dest, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("shot (screen): " + $dest)
    return $dest
}

# One screenshot by either path: frame dump first, screen capture on failure.
function Save-Shot {
    param([string]$ShotName)
    $p = Save-FrameDumpShot -ShotName $ShotName
    if ($p) { return $p }
    Write-Warning "Frame dump shot failed; falling back to screen capture."
    return (Save-ScreenShot -ShotName $ShotName)
}

# Collect the sampled-texture dumps (*_smp_*.bin) from every frame folder
# into the result folder. The atlas-dump build writes them next to the
# frame PNGs when KYTY_DBG_FRAME_DUMP_SAMPLED=1 is on; any capture (F2 or
# the trigger file) writes them, because OnDraw queues them during a
# capture. Waits briefly for the writes to land, then copies what is
# there. Returns the count copied (0 when the build has no sampled dump,
# e.g. a non-atlas build ran by mistake).
function Save-SampledBins {
    $dumpDir = $env:KYTY_DBG_FRAME_DUMP_DIR
    if (-not $dumpDir) { $dumpDir = "D:\Downloads\captures" }
    $found = @()
    for ($w = 0; $w -lt 6; $w++) {
        $found = @(Get-ChildItem -Path $dumpDir -Directory -Filter "frame_*" -ErrorAction SilentlyContinue |
            ForEach-Object { Get-ChildItem -Path $_.FullName -Filter "*_smp_*.bin" -ErrorAction SilentlyContinue } |
            Where-Object { $_ })
        if ($found.Count -gt 0) { break }
        Start-Sleep -Seconds 5
    }
    if ($found.Count -eq 0) {
        Write-Warning "No *_smp_*.bin files found in the frame folders. The build may not be the atlas-dump build, or KYTY_DBG_FRAME_DUMP_SAMPLED was not on."
        return 0
    }
    $copied = 0
    foreach ($f in $found) {
        try {
            Copy-Item -Path $f.FullName -Destination (Join-Path $OutDir $f.Name) -Force -ErrorAction Stop
            $copied++
        } catch {
            Write-Warning ("Could not copy sampled bin " + $f.Name + ": " + $_.Exception.Message)
        }
    }
    Write-Host ("sampled bins: " + $copied + " copied to the result folder")
    return $copied
}

# OCR a screenshot to text (empty string when Tesseract is missing).
function Get-ShotText {
    param([string]$ShotPath)
    if (-not $script:TesseractExe) { return "" }
    if (-not (Test-Path $ShotPath)) { return "" }
    $tmp = Join-Path $OutDir "ocr-tmp.txt"
    # v28: downscale the 4K screenshot to 1080p before OCR. Tesseract on
    # a full 3840x2160 image is slow and CPU-heavy, and it starves the
    # emulator: in the 1316 run the FPS collapsed from 60 to 5-6 during
    # Versus navigation, when the script runs OCR on every nav shot, and
    # never recovered. Manual play never hits this because no OCR runs at
    # all. The UI text the classifier needs is large and reads fine at
    # 1080p. Falls back to the full-res image if downscaling fails.
    $smallPath = Join-Path $OutDir "ocr-small.png"
    try {
        Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
        $src = [System.Drawing.Bitmap]::FromFile($ShotPath)
        $small = New-Object System.Drawing.Bitmap(1920, 1080)
        $g = [System.Drawing.Graphics]::FromImage($small)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($src, 0, 0, 1920, 1080)
        $g.Dispose()
        $src.Dispose()
        $small.Save($smallPath, [System.Drawing.Imaging.ImageFormat]::Png)
        $small.Dispose()
        $ShotPath = $smallPath
    } catch { }
    try {
        & $script:TesseractExe $ShotPath $tmp.Replace(".txt", "") --psm 6 2>$null | Out-Null
        if (Test-Path $tmp) { return (Get-Content $tmp -Raw -ErrorAction SilentlyContinue) }
    } catch { }
    return ""
}

# Classify a screen from its OCR text. Returns one of:
# SignIn, Title, Hub, Versus, Ice, Pause, Unknown.
function Get-ScreenKind {
    param([string]$Text)
    $t = $Text.ToLower()
    if ($t -match "sign in|playstation network|psn|sign-in|ea servers|unable-to-connect") { return "SignIn" }
    if ($t -match "press any button|press x to start|press x") { return "Title" }
    # X-Factors screen (Be A Pro player abilities): the nav loop backs
    # out of it with Circle. Check before Hub, since the screen can
    # carry a "BE A PRO" header that the Hub pattern would swallow.
    if ($t -match "x-factor|x factor|xfactor") { return "XFactor" }
    # Be A Pro hub FIRST for "play next game": the hub preview headline
    # reads "Game Preview: ... Face-off Against ...", and the bare
    # "face-off" Versus marker below swallowed the hub (2258 run shot 11:
    # hub read as Versus, the loop jumped forward and never drove the
    # hub). "play next game" is distinct from the Play Now "play now"
    # the Versus-first ordering below protects, so this is safe.
    if ($t -match "play next game") { return "Hub" }
    # Versus FIRST: the versus screen header reads "PLAY NOW SELECT TEAM",
    # so the Hub "play now" pattern below would swallow it (v9/v10 atlas
    # run: shot 234-nav.png is Versus on screen yet classified Hub, and
    # the loop pressed Down+Cross on it ~240 times). Versus markers are
    # "select team" / "select sides" / "versus" / faceoff.
    if ($t -match "select team|select sides|versus|faceoff|face-off") { return "Versus" }
    # Be A Pro versus splash: two player panels, "VERSUS" written
    # vertically (OCR usually misses it) and "AWAY | 53-1-0" top-left.
    # The record is the reliable marker; OCR reads 0 as O, so allow O.
    if (($t -match "away") -and ($t -match "[0-9o]+\s*-\s*[0-9o]+\s*-\s*[0-9o]+")) { return "Versus" }
    # GameExperience BEFORE Ice: the dialog says "5 MINUTE PERIODS" and
    # Ice used to match bare "period" (v14, 1736 run: the dialog was read
    # as Ice and the nav loop quit before versus). Markers are the dialog
    # title and the period-length options; "minute periods" covers the
    # case where OCR reads only the options list, without the title or
    # the "(1-2 SHIFTS PER PERIOD)" parenthetical.
    if ($t -match "choose a game|game experience|shifts per period|minute periods") { return "GameExperience" }
    if ($t -match "be a pro|play now|play next game|franchise|hut|world of chel") { return "Hub" }
    if ($t -match "pause|paused|resume|quit to|settings") { return "Pause" }
    # Ice needs real HUD text. The bare words "period", "shots", "goals"
    # and "hits" matched menus: the GameExperience dialog says "5 MINUTE
    # PERIODS", the goal-summary screen says "GOAL SUMMARY". The scorebug
    # shows SOG and a clock with a period, so match those instead.
    if ($t -match "sog|power play|penalty kill") { return "Ice" }
    if (($t -match "\d+:\d+") -and ($t -match "1st|2nd|3rd")) { return "Ice" }
    return "Unknown"
}

# Keep the display awake for the test run: the overnight auto-tests keep
# dying at launch in vkGetPhysicalDeviceSurfaceCapabilitiesKHR
# (ErrorUnknown -13), which points at the display being off or asleep when
# the watcher launches the emulator - the same build runs fine manually
# with the monitor on. ES_DISPLAY_REQUIRED wakes a display that power
# management turned off and keeps it on while the test runs; it is cleared
# at the end of the script. A physically powered-off monitor is unaffected.
# Ported from the Nov 9 local commit, which never reached this branch.
$DisplayAwakeCode = @"
using System;
using System.Runtime.InteropServices;
public static class KytyDisplayAwake {
    [DllImport("kernel32.dll")]
    public static extern uint SetThreadExecutionState(uint esFlags);
    public static uint Wake() { return SetThreadExecutionState(0x80000003u); }
    public static uint Clear() { return SetThreadExecutionState(0x80000000u); }
}
"@
function Request-DisplayAwake {
    try { Add-Type -TypeDefinition $DisplayAwakeCode -ErrorAction Stop } catch { }
    try {
        [KytyDisplayAwake]::Wake() | Out-Null
        Write-Host "Display wake requested (display stays on for the test run)."
    } catch {
        Write-Warning ("Could not request display wake: " + $_.Exception.Message)
    }
}
function Clear-DisplayAwake {
    try { [KytyDisplayAwake]::Clear() | Out-Null } catch { }
}

# v27: bring the emulator window to the front before a screenshot. The
# screen capture grabs the foreground window, so if the browser or anything
# else is covering the game, the nav shots capture that instead and OCR
# reads the wrong screen (the Nov 9 background-task failure). A scheduled
# task cannot steal focus with SetForegroundWindow alone, so the helper
# uses the AttachThreadInput trick to attach to the foreground thread
# first. Returns $true when the emulator is in front afterwards.
$KytyForegroundCode = @"
using System;
using System.Runtime.InteropServices;
public static class KytyForeground {
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();
}
"@
function Invoke-BringEmulatorToFront {
    try { Add-Type -TypeDefinition $KytyForegroundCode -ErrorAction Stop } catch { }
    try {
        $emu = Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $emu) { return $false }
        $hwnd = $emu.MainWindowHandle
        if ($hwnd -eq [IntPtr]::Zero) { return $false }
        if ([KytyForeground]::IsIconic($hwnd)) {
            [KytyForeground]::ShowWindow($hwnd, 9) | Out-Null
        }
        $fgHwnd = [KytyForeground]::GetForegroundWindow()
        if ($fgHwnd -eq $hwnd) { return $true }
        $fgTid = 0
        [void][KytyForeground]::GetWindowThreadProcessId($fgHwnd, [ref]$fgTid)
        $myTid = [KytyForeground]::GetCurrentThreadId()
        [KytyForeground]::AttachThreadInput($myTid, $fgTid, $true) | Out-Null
        [KytyForeground]::SetForegroundWindow($hwnd) | Out-Null
        [KytyForeground]::AttachThreadInput($myTid, $fgTid, $false) | Out-Null
        Start-Sleep -Milliseconds 300
        return ([KytyForeground]::GetForegroundWindow() -eq $hwnd)
    } catch {
        return $false
    }
}

# Launch the emulator exactly the way the launcher does: the game path, the
# red-zone flag, --skip-notice-screen, and the full play resolution. The
# emulator never reads cfgGlobal.json (only the launcher applies it), so the
# frame dump is controlled purely by the process env vars set below, which
# reach the child via $psi.EnvironmentVariables. Start-Process inheritance
# was proven not to deliver KYTY_INPUT_FILE, so set the vars directly.
$emuArgs = @("--game", $GamePath, "--redzone", "--skip-notice-screen", "--screen-width", $ScreenWidth, "--screen-height", $ScreenHeight)
$argString = '--game "' + $GamePath + '" --redzone --skip-notice-screen --screen-width ' + $ScreenWidth + ' --screen-height ' + $ScreenHeight
Write-Host ("launch: " + $EmulatorExe + " " + $argString)
Write-Host ("screen: " + $ScreenWidth + "x" + $ScreenHeight + " (full res, matching manual play)")
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $EmulatorExe
$psi.Arguments = $argString
$psi.WorkingDirectory = $BuildDir
$psi.UseShellExecute = $false
# v25: redirect the emulator's stdout/stderr so the fatal report behind a
# boot exit is captured. Log::WriteFatal always prints to stdout, and a
# directly-launched emulator never writes last-run.log, so without this
# the report is lost entirely. Async readers drain the pipes while the
# script sleeps, so a chatty emulator can never block on a full pipe.
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.EnvironmentVariables["KYTY_INPUT_FILE"] = $InputFile
$psi.EnvironmentVariables["KYTY_COMPUTE_RESOLVE_AHEAD"] = "1"
$psi.EnvironmentVariables["KYTY_DBG_FRAME_DUMP"] = "1"
$psi.EnvironmentVariables["KYTY_DBG_FRAME_DUMP_DIR"] = "D:\Downloads\captures"
$psi.EnvironmentVariables["KYTY_DBG_FRAME_DUMP_CS_CONSTANTS"] = "1"
if ($AtlasCapture) {
    # Atlas-dump build: dump every sampled BC texture as *_smp_*.bin.
    $psi.EnvironmentVariables["KYTY_DBG_FRAME_DUMP_SAMPLED"] = "1"
    Write-Host "AtlasCapture: KYTY_DBG_FRAME_DUMP_SAMPLED=1 (sampled BC texture dump on)"
}
$psi.EnvironmentVariables["KYTY_DBG_LOOP_CAP"] = "65536"
$psi.EnvironmentVariables["KYTY_DBG_SKIP_BAD_SHADERS"] = "1"
$psi.EnvironmentVariables["KYTY_DBG_BREADCRUMBS"] = "0"
$psi.EnvironmentVariables["KYTY_DBG_CONTROLLERS"] = "1"
Write-Host ("KYTY_INPUT_FILE=" + $psi.EnvironmentVariables["KYTY_INPUT_FILE"])
# v24: fingerprint last-run.log BEFORE launch, so a boot exit can tell a
# fresh log from a stale one. The emulator does not rewrite the log when
# it exits during boot (the 09:16 run copied the 22:58 session's log).
$logSrc = Join-Path $env:LOCALAPPDATA "KyTyPS5\logs\last-run.log"
$logBefore = $null
if (Test-Path $logSrc) {
    $logBefore = Get-Item $logSrc
    Write-Host ("log before launch: " + $logBefore.Length + " bytes, modified " + $logBefore.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))
} else {
    Write-Host "log before launch: no last-run.log present"
}
# Wake the display before launching: a sleeping display is the prime
# suspect for the overnight vkGetPhysicalDeviceSurfaceCapabilitiesKHR
# launch crashes (see Request-DisplayAwake above).
Request-DisplayAwake
# v26: log the pre-launch machine state, then give the display time to
# come up after the wake request before launching. v25's wake ran on the
# 10:16 run and the emulator still died about 1 s after launch in
# vkGetPhysicalDeviceSurfaceCapabilitiesKHR (ErrorUnknown -13): the wake
# request starts the wake, but the display path may not be ready when the
# first surface query lands. A locked workstation (logonui running) leaves
# no usable surface at all - that case needs the user, not a longer wait.
$LastInputCode = @"
using System;
using System.Runtime.InteropServices;
public static class KytyLastInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
    [DllImport("user32.dll")]
    public static extern bool GetLastInputInfo(ref LASTINPUTINFO plii);
    [DllImport("kernel32.dll")]
    public static extern uint GetTickCount();
    public static uint IdleMilliseconds() {
        LASTINPUTINFO lii = new LASTINPUTINFO();
        lii.cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(LASTINPUTINFO));
        if (!GetLastInputInfo(ref lii)) return uint.MaxValue;
        uint now = GetTickCount();
        return now - lii.dwTime;
    }
}
"@
$locked = Get-Process -Name "logonui" -ErrorAction SilentlyContinue
if ($locked) { Write-Warning "Workstation is LOCKED (logonui running) - the emulator may not get a usable display surface." }
else { Write-Host "Workstation unlocked (no logonui process)." }
try {
    Add-Type -TypeDefinition $LastInputCode -ErrorAction Stop
    $idleMs = [KytyLastInput]::IdleMilliseconds()
    if ($idleMs -eq [uint32]::MaxValue) { Write-Host "Idle time: unknown (GetLastInputInfo failed)." }
    else { Write-Host ("Idle time since last input: " + [math]::Round($idleMs / 1000) + " s") }
} catch {
    Write-Host "Idle time: unknown (could not query last input)."
}
Write-Host "Waiting 10 s after the display wake so the display path is up before launch."
Start-Sleep -Seconds 10
# Log any kyty_emulator process already running before launch, so a later
# launch crash can be told apart from a leftover-process conflict. The
# watcher normally skips when one is running; this line closes the loop
# for runs started any other way.
$preExisting = Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue
foreach ($p in $preExisting) {
    $started = "unknown"
    try { $started = $p.StartTime.ToString() } catch { }
    Write-Warning ("kyty_emulator already running before launch: PID " + $p.Id + " started " + $started)
}
$stdoutLog = Join-Path $OutDir "emulator-stdout.log"
$stderrLog = Join-Path $OutDir "emulator-stderr.log"
$proc = [System.Diagnostics.Process]::Start($psi)
Write-Host ("pid: " + $proc.Id)
# v25: start the async stdout/stderr readers. Register-ObjectEvent drains
# the pipes on the event manager while the script sleeps or navigates
# (sandbox-proven: 200,000 stdout lines + 5,000 stderr lines captured
# with no deadlock while the child flooded output). If registration fails,
# stop the emulator and fail fast: a redirected process with no reader
# would block once the pipe fills, and a crippled run proves nothing.
try {
    $stdoutSub = Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -MessageData $stdoutLog -Action { if ($EventArgs.Data -ne $null) { [System.IO.File]::AppendAllText($Event.MessageData, $EventArgs.Data + "`r`n") } }
    $stderrSub = Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -MessageData $stderrLog -Action { if ($EventArgs.Data -ne $null) { [System.IO.File]::AppendAllText($Event.MessageData, $EventArgs.Data + "`r`n") } }
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()
} catch {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    throw ("Could not start emulator output capture: " + $_.Exception.Message)
}

# Boot wait: the game needs the full time to load the title screen and the
# sign-in dialog before input means anything. Fail fast if the emulator died
# during the wait: the nav loop would otherwise spend ~25 minutes taking
# desktop screenshots of a dead process.
Write-Host ("boot wait: " + $BootWaitSec + " s (the game needs the full load before input means anything)")
Start-Sleep -Seconds $BootWaitSec
$EmulatorDead = -not (Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue)
if ($EmulatorDead) {
    Write-Warning "Emulator exited during the boot wait, before any input was sent. Stopping here - see last-run.log for why it exited."
    # v24: the exit code is the one fact a boot exit always leaves behind,
    # and the log fingerprint tells a fresh log from a stale one. A blind
    # copy wastes the upload and misleads the monitor (09:16 copied the
    # 22:58 session's log). A stale log is labelled and not copied.
    if ($proc.HasExited) {
        Write-Host ("emulator exit code: " + $proc.ExitCode + " (decimal; 321 = EXIT_HALT fatal assert, -1073741819 = 0xC0000005 access violation, 0 = clean exit)")
        Write-Host ("emulator exit time: " + $proc.ExitTime.ToString("yyyy-MM-dd HH:mm:ss"))
    } else {
        Write-Host "emulator exit code: process object still reports running (name check disagrees - see transcript)"
    }
    if (Test-Path $logSrc) {
        $logAfter = Get-Item $logSrc
        $logFresh = $false
        if ($logBefore -ne $null) {
            $logFresh = ($logAfter.LastWriteTime -gt $logBefore.LastWriteTime) -or ($logAfter.Length -ne $logBefore.Length)
        } else {
            $logFresh = $true
        }
        Write-Host ("log after exit: " + $logAfter.Length + " bytes, modified " + $logAfter.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss") + ", fresh=" + $logFresh)
        if ($logFresh) {
            Copy-Item -Path $logSrc -Destination (Join-Path $OutDir "last-run.log") -Force -ErrorAction SilentlyContinue
            Write-Host "log: copied last-run.log to the result folder"
        } else {
            Write-Host "log: STALE (unchanged since before launch) - not copied; it is a previous session, not this boot exit"
        }
    } else {
        Write-Host "log: no last-run.log present after the boot exit"
    }
    # v25: stop the async readers so emulator-stdout.log / emulator-stderr.log
    # are complete before the watcher copies the result folder. The 2s pause
    # lets the event queue drain trailing lines after the process exited.
    # The fatal report behind this boot exit is in emulator-stdout.log.
    Start-Sleep -Seconds 2
    try { $proc.CancelOutputRead() } catch { }
    try { $proc.CancelErrorRead() } catch { }
    if ($stdoutSub -ne $null) { Unregister-Event -SubscriptionId $stdoutSub.Id -ErrorAction SilentlyContinue }
    if ($stderrSub -ne $null) { Unregister-Event -SubscriptionId $stderrSub.Id -ErrorAction SilentlyContinue }
    Clear-DisplayAwake
    Stop-Transcript | Out-Null
    exit 1
}

# STEP 1 - dismiss the PSN sign-in dialog, explicitly, before any title
# input. The dialog sits on top of the title screen and eats Cross presses
# (the 2026-11-09 1204 run proved it: title "Press any button" visible
# behind the dialog, Cross pressed, game never advanced). Circle is the
# PlayStation back/cancel button. Do it up to 3 times, re-reading after
# each, and only move on once the dialog is gone.
$signInGone = $false
for ($s = 1; $s -le 3; $s++) {
    $shot = Save-ScreenShot -ShotName ("00-signin-" + $s + ".png")
    $text = Get-ShotText -ShotPath $shot
    $kind = Get-ScreenKind -Text $text
    Write-Host ("signin check " + $s + ": " + $kind)
    if ($kind -ne "SignIn") { $signInGone = $true; break }
    # Alternate Circle / Cross: the PSN dialog needs Circle (back), the EA
    # servers dialog needs Cross (acknowledge). Circle alone failed 3
    # times on the EA dialog in the v7 atlas run.
    if (($s % 2) -eq 1) {
        Write-Host "Sign-in dialog is up - pressing Circle to dismiss it."
        Invoke-InputStage -Lines @("l 150", "sleep 3000") -ExtraWaitSec 2
    } else {
        Write-Host "Sign-in dialog still up - pressing Cross to acknowledge it."
        Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
    }
}
if (-not $signInGone) {
    Write-Warning "Sign-in dialog would not dismiss after 3 tries; continuing anyway - the title nav below may still work."
}

# STEP 2 - roadmap navigation. The script follows a fixed route, not a
# guess: Title -> Hub -> GameExperience -> Versus -> Ice. It always knows which stop it is
# heading for, and only accepts the screen it expects (or a known detour).
# A reactive loop that presses buttons on whatever OCR reads gets lost:
# it pressed Cross on the main menu with BE A PRO CAREER highlighted and
# opened Be A Pro instead of Play Now (v7 atlas run), and it read a dark
# loading screen as Ice because "nhl" sits in the title bar (v7 run).
# The roadmap fixes both: each stop names the screen it expects and the
# one action that advances from it. Anything else is a detour to handle,
# not a screen to act on:
#  - SignIn dialog (PSN or EA servers): dismiss, stay on the same stop.
#  - Pause menu: resume, stay on the same stop.
#  - Unknown (loading screen, no text): wait and re-read; never press a
#    button on a screen it cannot name.
#  - A LATER stop than expected (the game advanced on its own): jump to it.
#  - An EARLIER stop than expected (the game went back): go back to it.
#  - A screen that matches no stop: wait and re-read.
# Each stop gets 8 tries, counted only on RECOGNIZED screens (v14): an
# Unknown loading screen waits and re-reads without consuming a try, or
# the cap fires mid-load as it did in the 1651/1706 runs. If a stop
# cannot be reached, the run fails with the stop name and the last screen
# seen, instead of wandering. Be A Pro
# rule (unchanged): if the screen offers only START CAREER with no
# CONTINUE, STOP - never create a character.
$roadmap = @("Title", "Hub", "GameExperience", "Versus", "Ice")
$step = 0
$navShot = 0
$signInTries = 0
$stepTries = 0
$unknownStreak = 0
$titleTries = 0
$reachedIce = $false
$reachedVersus = $false
$navFailed = $false
# Loading-pace tracking (v18): the frame counter in the overlay status
# line ("frame: N, fps: M") tells whether the game is loading. A shot
# that shows no new frame, or fps 2 or less, is a wait, not navigation.
$lastFrame = -1
$frozenStreak = 0
$slowStreak = 0
$navBudgetUsed = 0
$navStart = Get-Date
# Global cap: the jump resets the per-stop counter, so a stuck game can
# ping-pong between two stops forever (245 shots in the v9 atlas run).
# 60 counted nav shots is far more than a clean run needs (a good run
# takes under 20); past that, fail naming the stop it stuck on. Shots
# taken at loading pace do not count (v18) - the 2258 run spent 11 shots
# on one frozen frame and ~27 more on a 0-2 fps cinematic. A 25 minute
# nav deadline is the backstop for a loop that never reaches a stop.
$maxNavShots = 60
while ($step -lt $roadmap.Count) {
    if ($navBudgetUsed -ge $maxNavShots) {
        Write-Warning ("Roadmap exceeded " + $maxNavShots + " counted nav shots at stop '" + $roadmap[$step] + "'. Stopping - see the nav shots and transcript for the last screen seen.")
        $navFailed = $true
        break
    }
    if (((Get-Date) - $navStart).TotalMinutes -ge 25) {
        Write-Warning ("Roadmap exceeded 25 minutes at stop '" + $roadmap[$step] + "'. Stopping - see the nav shots and transcript for the last screen seen.")
        $navFailed = $true
        break
    }
    if ($stepTries -gt 8) {
        Write-Warning ("Roadmap stuck at stop '" + $roadmap[$step] + "' after 8 tries. Stopping - see the nav shots and transcript for the last screen seen.")
        $navFailed = $true
        break
    }
    if (-not (Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue)) {
        Write-Warning "Emulator exited mid-navigation. Stopping the nav loop - see last-run.log."
        $navFailed = $true
        break
    }
    $navShot++
    $shotName = ("{0:D2}-nav.png" -f $navShot)
    $shot = Save-ScreenShot -ShotName $shotName
    $text = Get-ShotText -ShotPath $shot
    $kind = Get-ScreenKind -Text $text
    # Loading pace from the overlay status line ("frame: N, fps: M").
    # Parse it here, before any other -match in this scope clobbers
    # $Matches. A shot at loading pace is a wait, not navigation, so it
    # does not spend the 60-shot budget (v18).
    $frameNow = -1
    $fpsNow = -1
    if ($text -match "frame:\s*([0-9]+)") { $frameNow = [int]$Matches[1] }
    if ($text -match "fps:\s*([0-9]+)") { $fpsNow = [int]$Matches[1] }
    $loadingPace = $false
    if ($frameNow -ge 0) {
        if ($frameNow -eq $lastFrame) { $frozenStreak++ } else { $frozenStreak = 0 }
        if (($fpsNow -ge 0) -and ($fpsNow -le 2)) { $slowStreak++ } else { $slowStreak = 0 }
        if (($frozenStreak -gt 0) -or ($slowStreak -gt 0)) { $loadingPace = $true }
        $lastFrame = $frameNow
    }
    if (-not $loadingPace) { $navBudgetUsed++ }
    $want = $roadmap[$step]
    Write-Host ("roadmap: stop '" + $want + "' (try " + $stepTries + "), screen: " + $kind + " (shot " + $shotName + ")")
    if ($text) { Write-Host ("ocr: " + (($text -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 3) -join " | ")) }

    # Known detours: handled the same at every stop, never advance the step.
    if ($kind -eq "SignIn") {
        $signInTries++
        # Alternate Cross / Circle: the EA servers dialog needs Cross
        # (acknowledge), the PSN dialog needs Circle (back).
        if (($signInTries % 2) -eq 1) {
            Write-Host "Sign-in dialog up - pressing Cross to acknowledge it."
            Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
        } else {
            Write-Host "Sign-in dialog still up - pressing Circle to dismiss it."
            Invoke-InputStage -Lines @("l 150", "sleep 3000") -ExtraWaitSec 2
        }
        continue
    }
    if ($kind -eq "Pause") {
        Write-Host "Pause menu - resuming with Cross."
        Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
        continue
    }
    if ($kind -eq "XFactor") {
        # The Be A Pro X-Factors screen: a player-abilities menu the
        # nav loop wandered into (the 2026-11-11 v28 run sat on it -
        # Unknown just waits, so it never backed out). Circle backs out
        # to the hub; re-read after.
        Write-Host "X-Factors screen - backing out with Circle."
        Invoke-InputStage -Lines @("l 150", "sleep 3000") -ExtraWaitSec 2
        continue
    }
    if ($kind -eq "Unknown") {
        # A loading screen changes on its own. Wait first, press nothing.
        # Only after 3 in a row press Cross once; after 5, Cross + Options
        # for a possible unnamed dialog. Then re-read.
        $unknownStreak++
        if ($loadingPace) {
            # Loading or cinematic at 0-2 fps: the frame is barely moving,
            # so pressing buttons cannot help - wait it out (this shot did
            # not spend the nav budget). 30 frozen shots in a row (about
            # 5 minutes at these waits) means genuinely stuck, not loading.
            if ($frozenStreak -gt 30) {
                Write-Warning ("Frame frozen at " + $frameNow + " for 30 shots - game is stuck, not loading. Stopping - see the nav shots and transcript for the last screen seen.")
                $navFailed = $true
                break
            }
            Write-Host ("Loading pace (frame " + $frameNow + ", fps " + $fpsNow + ") - waiting 10 s and re-reading (may be a loading screen or cinematic).")
            Start-Sleep -Seconds 10
        } elseif ($unknownStreak -le 2) {
            Write-Host "Screen not recognized - waiting 4 s and re-reading (may be a loading screen)."
            Start-Sleep -Seconds 4
        } elseif ($unknownStreak -eq 3) {
            Write-Host "Screen still not recognized - pressing confirm once and re-reading."
            Invoke-InputStage -Lines @("j 150", "sleep 2000") -ExtraWaitSec 2
        } else {
            Write-Host "Screen still not recognized - dismissing a possible dialog with Cross then Options."
            Invoke-InputStage -Lines @("j 150", "sleep 1500", "Return 150", "sleep 3000") -ExtraWaitSec 2
            $unknownStreak = 0
        }
        continue
    }
    $unknownStreak = 0
    # Only a recognized screen consumes a try (v14): an Unknown loading
    # screen above already continued without reaching this line, so a
    # slow hub load no longer burns the 8-try budget on waits.
    $stepTries++

    # Where does this screen sit on the roadmap? Ahead means the game
    # advanced on its own (jump forward); behind means it went back.
    $seen = [Array]::IndexOf($roadmap, $kind)
    if ($seen -ge 0 -and $seen -ne $step) {
        if ($seen -gt $step) {
            Write-Host ("Screen is a later stop ('" + $kind + "') - jumping forward to it.")
        } else {
            Write-Host ("Screen is an earlier stop ('" + $kind + "') - going back to it.")
        }
        $step = $seen
        # Do NOT reset $stepTries here: v9 reset it on every jump back, so
        # the 8-tries-per-stop cap never fired during a Hub<->Versus
        # oscillation. The v10 global 60-shot cap stays as the backstop.
        $want = $roadmap[$step]
    }
    if ($kind -ne $want) {
        Write-Host ("Screen '" + $kind + "' is not the stop '" + $want + "' - waiting and re-reading.")
        Start-Sleep -Seconds 4
        continue
    }

    # We are at the expected stop. Take its one action, then advance.
    $stepTries = 0
    if ($want -eq "Title") {
        $titleTries++
        if (($titleTries % 2) -eq 1) {
            Write-Host "Title screen - pressing Cross to reach the main menu."
            Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
        } else {
            Write-Host "Title screen - Cross did not advance it; pressing Options (Start) instead."
            Invoke-InputStage -Lines @("Return 150", "sleep 3000") -ExtraWaitSec 2
        }
        $step++
        continue
    }
    if ($want -eq "Hub") {
        # Be A Pro guard: START CAREER with no CONTINUE means no save yet;
        # creating a character is forbidden, so stop here.
        if (($text -match "start career") -and ($text -notmatch "continue")) {
            Write-Warning "Be A Pro offers only START CAREER (no CONTINUE) - stopping, never creating a character."
            $navFailed = $true
            break
        }
        # Two different Hub screens. The Be A Pro hub highlights PLAY NEXT
        # GAME, so Cross is right there. The main menu highlights BE A PRO
        # CAREER first, so Cross alone opens Be A Pro - that is the route
        # we want (v12): the versus screen that matters is the Be A Pro
        # one, not Play Now. Do NOT press Down to PLAY NOW. Tell the two
        # Hub screens apart by the OCR text.
        if ($text -match "play next game") {
            Write-Host "Be A Pro hub - selecting PLAY NEXT GAME for the Versus screen."
            Invoke-InputStage -Lines @("j 150", "sleep 4000") -ExtraWaitSec 2
        } else {
            Write-Host "Main menu - selecting BE A PRO CAREER (default highlight) for the Be A Pro Versus screen."
            Invoke-InputStage -Lines @("j 150", "sleep 4000") -ExtraWaitSec 2
        }
        $step++
        continue
    }
    if ($want -eq "GameExperience") {
        # The game asks which game experience to play. Confirm the
        # highlighted choice (CUSTOM, the default) - the same choice the
        # 1736 run confirmed, which reached versus and the ice. Do NOT
        # move the highlight: changing the preset is the user's call.
        Write-Host "Game experience dialog - confirming the highlighted choice for the Versus screen."
        Invoke-InputStage -Lines @("j 150", "sleep 4000") -ExtraWaitSec 2
        $step++
        continue
    }
    if ($want -eq "Versus") {
        if (-not $reachedVersus) {
            $reachedVersus = $true
            # The versus capture is the one frame dump that matters (it
            # writes the sampled .bin files). Wait for any earlier capture
            # to finish downloading first, or the trigger is swallowed.
            Wait-FrameDumpQuiescent
            $v = Save-Shot -ShotName "04-versus.png"
            Write-Host ("versus screen captured: " + $v)
            if ($AtlasCapture) {
                # Atlas capture: the versus dump already wrote the sampled
                # .bin files; collect them, then stop. No gameplay.
                $binCount = Save-SampledBins
                if ($binCount -gt 0) {
                    Write-Host ("AtlasCapture done: " + $binCount + " sampled bins in the result folder. Stopping here.")
                } else {
                    Write-Warning "AtlasCapture: versus captured but no sampled bins found - check the build is the atlas-dump build (#139, 4b197fb)."
                }
                break
            }
        }
        Write-Host "Versus screen - pressing confirm to start the game."
        Invoke-InputStage -Lines @("j 150", "sleep 5000") -ExtraWaitSec 2
        $step++
        continue
    }
    if ($want -eq "Ice") {
        $reachedIce = $true
        Write-Host "On the ice - roadmap complete, leaving the nav loop for gameplay shots."
        $step++
        continue
    }
}
if ($navFailed) {
    Write-Warning "Navigation did not complete the roadmap. Result folder holds the nav shots and the transcript for diagnosis."
}

# STEP 3 - gameplay shots (only if the nav reached the ice). 05/06 are the
# opening ice shots, 07 is the pause menu, 08 is the resumed ice, and
# 09/10/11 are extended-play interval shots that catch transitions
# (faceoff, goal, replay, period changes). Skipped for -AtlasCapture
# (the run already stopped at the versus screen).
if ($reachedIce -and -not $SkipGameplay -and -not $AtlasCapture) {
    $g5 = Save-ScreenShot -ShotName "05-ice.png"
    Write-Host ("ice captured: " + $g5)
    Write-Host "Skating (left stick up) for a gameplay shot."
    Invoke-InputStage -Lines @("w 2000", "sleep 1000") -ExtraWaitSec 1
    $g6 = Save-ScreenShot -ShotName "06-ice-skate.png"
    Write-Host ("ice skate captured: " + $g6)
    Write-Host "Opening the pause menu (Options)."
    Invoke-InputStage -Lines @("Return 150", "sleep 3000") -ExtraWaitSec 2
    $g7 = Save-ScreenShot -ShotName "07-pause-menu.png"
    $g7Text = Get-ShotText -ShotPath $g7
    $g7Kind = Get-ScreenKind -Text $g7Text
    if ($g7Kind -eq "Pause") {
        Write-Host ("pause menu captured: " + $g7)
    } else {
        Write-Warning ("Options did not open the pause menu - screen is " + $g7Kind + ", so 07-pause-menu.png is NOT a pause menu.")
    }
    Write-Host "Resuming from the pause menu (Cross)."
    Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
    $g8 = Save-ScreenShot -ShotName "08-ice-resumed.png"
    Write-Host ("ice resumed captured: " + $g8)
    for ($p = 1; $p -le 3; $p++) {
        Write-Host ("Extended play interval " + $p + " of 3 (skating, then a shot).")
        Invoke-InputStage -Lines @("w 2000", "sleep 1000", "j 150", "sleep 4000") -ExtraWaitSec 2
        $gp = Save-ScreenShot -ShotName ("{0:D2}-ice-play-{1}.png" -f (8 + $p), $p)
        Write-Host ("extended play captured: " + $gp)
    }
    # Only claim the shots that were actually taken: v14 claimed
    # "04-versus ... 07 pause menu" unconditionally, but the 1736 run
    # never reached the Versus stop and 07 was a dialog, not a menu.
    $doneMsg = "Gameplay shots complete: 05/06/08/09/10/11 ice"
    if ($reachedVersus) { $doneMsg += ", 04-versus" } else { $doneMsg += " (04-versus was never captured - Versus stop not reached)" }
    if ($g7Kind -eq "Pause") { $doneMsg += ", 07 pause menu" } else { $doneMsg += " (07 is not a pause menu - see warning above)" }
    Write-Host $doneMsg
} elseif ($reachedIce) {
    Write-Host "Reached the ice; gameplay shots skipped (-SkipGameplay)."
} else {
    Write-Warning "Did not reach the ice in the nav loop. Result folder holds the nav shots and the transcript for diagnosis."
}

# STEP 4 - dumb full-game gameplay (v4). The point is coverage, not skill:
# keep the player moving and the game progressing so the emulator collects
# gameplay data (logs, frame dumps, crash data) through a full game, even
# if the play is bad. The game auto-changes lines, so shift changes and
# powerplays happen on their own - exactly the coverage the missing-players
# crash needs. Runs for up to $GameplayMinutes real minutes (default 90,
# a full game at real-time pace with stoppages), or until the game ends
# (OCR shows Hub/Versus/Title/SignIn, meaning back to a menu). Each cycle
# sends a varied input pattern (skate, pass, shoot, poke). The screen is
# OCR-checked every few cycles from a fast screen capture: Pause is
# resumed with Cross, a menu means the game is over, Unknown is tolerated
# and logged. A frame-dump data shot is taken every 10 minutes, and a
# fast screen capture (flash-watch, 12-flash-<n>.png) every 5 cycles
# (about 30 seconds) to catch brief glitches the 10-minute shots miss.
# The emulator exiting (crash) ends the phase immediately. Skipped for
# -AtlasCapture (the run already stopped at the versus screen).
if ($reachedIce -and -not $SkipGameplay -and -not $AtlasCapture) {
    $gameplayStart = Get-Date
    $gameplayEnd = $gameplayStart.AddMinutes($GameplayMinutes)
    $gameOver = $false
    $cycle = 0
    $unknownStreak = 0
    $gameplayShotCount = 0
    $flashShotCount = 0
    $lastDataShot = $gameplayStart
    Write-Host ("gameplay phase: up to " + $GameplayMinutes + " min of dumb play, ends early if the game returns to a menu.")
    while (((Get-Date) -lt $gameplayEnd) -and -not $gameOver) {
        $cycle++
        if (-not (Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue)) {
            Write-Warning ("Emulator exited during gameplay (cycle " + $cycle + "). Stopping the gameplay phase - see last-run.log.")
            break
        }
        # Varied input pattern, rotating by cycle. Left stick (w/a/s/d)
        # skates, Cross (j) passes and takes faceoffs, Circle (l) shoots,
        # Square (k) poke-checks, D-pad Up changes lines. No smart play -
        # just keep moving and keep the puck in play so the game progresses.
        $pattern = $cycle % 6
        if ($pattern -eq 0) {
            $lines = @("w 1800", "sleep 300", "j 150", "sleep 500", "w 1200", "sleep 500")
        } elseif ($pattern -eq 1) {
            $lines = @("a 1500", "sleep 300", "l 150", "sleep 500", "w 1200", "sleep 500")
        } elseif ($pattern -eq 2) {
            $lines = @("d 1500", "sleep 300", "j 150", "sleep 500", "s 1200", "sleep 500")
        } elseif ($pattern -eq 3) {
            $lines = @("s 1500", "sleep 300", "l 150", "sleep 500", "w 1200", "sleep 500")
        } elseif ($pattern -eq 4) {
            $lines = @("k 150", "sleep 300", "j 150", "sleep 500", "w 1800", "sleep 500")
        } else {
            $lines = @("Up 150", "sleep 300", "w 2000", "sleep 300", "j 150", "sleep 500")
        }
        Invoke-InputStage -Lines $lines -ExtraWaitSec 1
        # Every 12 cycles (about a minute), OCR-check the screen from a
        # fast screen capture and react. The frame-dump data shot below is
        # separate; this check only needs to read the screen state.
        # (v17: was every 6 cycles; Tesseract every 30 seconds cost CPU
        # and slowed the game for no benefit - Pause/menu/game-over do
        # not need a 30-second response.)
        if (($cycle % 12) -eq 0) {
            $checkShot = Save-ScreenShot -ShotName "12-gameplay-check.png"
            $checkText = Get-ShotText -ShotPath $checkShot
            $checkKind = Get-ScreenKind -Text $checkText
            Write-Host ("gameplay check (cycle " + $cycle + "): " + $checkKind)
            if ($checkKind -ne "Unknown") { $unknownStreak = 0 }
            if ($checkKind -eq "Pause") {
                Write-Host "Pause menu during gameplay - resuming with Cross."
                Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
            } elseif (($checkKind -eq "Hub") -or ($checkKind -eq "Versus") -or ($checkKind -eq "Title") -or ($checkKind -eq "SignIn")) {
                Write-Host ("Gameplay phase: screen is " + $checkKind + " - the game has returned to a menu, so the game is over. Ending the gameplay phase.")
                $gameOver = $true
            } elseif ($checkKind -eq "Unknown") {
                $unknownStreak++
                if ($unknownStreak -ge 5) {
                    Write-Warning ("Gameplay screen not recognized for 5 checks in a row (cycle " + $cycle + "). The game may be in a replay, cutscene, or a state OCR cannot name. Continuing to send inputs.")
                    $unknownStreak = 0
                }
            }
            # Ice needs no action - the pattern keeps the game moving.
        }
        # Frame-dump data shot every 10 minutes.
        if (((Get-Date) - $lastDataShot).TotalMinutes -ge 10) {
            $gameplayShotCount++
            $lastDataShot = Get-Date
            $ds = Save-Shot -ShotName ("12-gameplay-" + $gameplayShotCount + ".png")
            Write-Host ("gameplay data shot: " + $ds)
        }
        # Flash-watch: fast screen capture every 5 cycles (about 30
        # seconds) to catch brief/transient glitches the 10-minute
        # frame-dump shots would miss. Screen capture only - no frame
        # dump, so it is cheap and does not disturb play.
        if (($cycle % 5) -eq 0) {
            $flashShotCount++
            $fs = Save-ScreenShot -ShotName ("12-flash-" + $flashShotCount + ".png")
            Write-Host ("flash-watch shot: " + $fs)
        }
        # Progress line every 30 cycles so the transcript shows the phase
        # is alive without flooding it.
        if (($cycle % 30) -eq 0) {
            $elapsed = [math]::Round(((Get-Date) - $gameplayStart).TotalMinutes, 1)
            Write-Host ("gameplay progress: cycle " + $cycle + ", " + $elapsed + " min elapsed of up to " + $GameplayMinutes + " min.")
        }
    }
    $gameplayMins = [math]::Round(((Get-Date) - $gameplayStart).TotalMinutes, 1)
    Write-Host ("gameplay phase done: " + $cycle + " cycles, " + $gameplayMins + " min, " + $gameplayShotCount + " data shots, " + $flashShotCount + " flash-watch shots.")
}

# Copy the emulator log into the result folder for diagnosis.
$logSrc = Join-Path $env:LOCALAPPDATA "KytyPS5\logs\last-run.log"
if (Test-Path $logSrc) {
    Copy-Item -Path $logSrc -Destination (Join-Path $OutDir "last-run.log") -Force -ErrorAction SilentlyContinue
    Write-Host "log: copied last-run.log to the result folder"
}

# Cleanup: leave the emulator running only when asked; otherwise stop it so
# the next watcher pass (or the user) starts clean.
if ($KeepRunning) {
    Write-Host "Keeping the emulator running (-KeepRunning)."
} else {
    $emu = Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue
    if ($emu) {
        Stop-Process -Id $emu.Id -Force -ErrorAction SilentlyContinue
        Write-Host "Emulator stopped."
    }
    # v25: stop the async stdout/stderr readers so the capture logs are
    # complete before the watcher copies the result folder. The 2s pause
    # lets the event queue drain trailing lines after the process exited.
    Start-Sleep -Seconds 2
    try { $proc.CancelOutputRead() } catch { }
    try { $proc.CancelErrorRead() } catch { }
    if ($stdoutSub -ne $null) { Unregister-Event -SubscriptionId $stdoutSub.Id -ErrorAction SilentlyContinue }
    if ($stderrSub -ne $null) { Unregister-Event -SubscriptionId $stderrSub.Id -ErrorAction SilentlyContinue }
}
Clear-DisplayAwake
Stop-Transcript | Out-Null
Write-Host ("done. result folder: " + $OutDir)
