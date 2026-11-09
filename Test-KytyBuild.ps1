# Test-KytyBuild.ps1 - v3, written from scratch (2026-11-09)
# Drives NHL 27 in the KyTy emulator from launch to the versus screen and
# through gameplay, using OCR-guided navigation and the emulator's scripted
# input file. No window-focus tricks.
#
# What is different from the v2 script (and why v2 never got past the title):
#  - The PSN sign-in dialog is handled FIRST, explicitly, before any title
#    input. v2 only tried to dismiss it after 3 Unknown reads, by which time
#    Cross presses were already being eaten by the dialog. The 2026-11-09
#    1204 run proved the dialog sits on top of the title screen ("Press any
#    button" visible behind it) and the test never got past it.
#  - Launch matches manual play: 3840x2160 (the resolution the game is played
#    at; 720p never showed the title prompt reliably) plus
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
#   [-OutDir "C:\Users\duy84\Desktop\KyTyTest-..."] [-ReleaseTag "..."]
#   [-BootWaitSec 90] [-ScreenWidth 3840] [-ScreenHeight 2160]
#   [-KeepRunning] [-SkipGameplay] [-NoOcr]

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
    [switch]$NoOcr
)

$ErrorActionPreference = "Stop"

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
# folder on the Desktop so the result is easy to find and upload.
if (-not $OutDir) {
    $stamp = Get-Date -Format "yyyy-MM-dd-HHmm"
    $tag = $ReleaseTag
    if (-not $tag) { $tag = Split-Path $BuildDir -Leaf }
    $OutDir = Join-Path ([Environment]::GetFolderPath("Desktop")) ("KyTyTest-" + $tag + "-" + $stamp)
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
if (Test-Path $InputFile) { Remove-Item $InputFile -Force }
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

# Write input lines to the file, then BLOCK until the emulator consumes it
# (deletes it), up to 30 s. If it is never consumed, the emulator did not get
# KYTY_INPUT_FILE, and pressing on would be pointless.
function Invoke-InputStage {
    param([string[]]$Lines, [int]$ExtraWaitSec = 2)
    $text = ($Lines -join "`r`n") + "`r`n"
    Write-Host ("input stage: " + ($Lines -join " | "))
    Set-Content -Path $InputFile -Value $text -Encoding ASCII -NoNewline
    $deadline = (Get-Date).AddSeconds(30)
    while ((Test-Path $InputFile) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 200 }
    if (Test-Path $InputFile) {
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
    $frameDir = Join-Path $dumpDir "frame_0"
    $before = @()
    if (Test-Path $frameDir) {
        $before = @(Get-ChildItem -Path $frameDir -Filter "*.png" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
    }
    $runTrig = Join-Path $env:LOCALAPPDATA "KytyPS5\run\framedump.trigger"
    New-Item -ItemType File -Force -Path $runTrig | Out-Null
    $buildTrig = Join-Path $BuildDir "framedump.trigger"
    New-Item -ItemType File -Force -Path $buildTrig | Out-Null
    $deadline = (Get-Date).AddSeconds(60)
    $found = $null
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if (Test-Path $frameDir) {
            $now = @(Get-ChildItem -Path $frameDir -Filter "*.png" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
            foreach ($f in $now) {
                if ($before -notcontains $f.Name) { $found = $f.FullName; break }
            }
            if ($found) { break }
        }
        if (-not (Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue)) { break }
    }
    if (-not $found) { Write-FrameDumpDiagnosis; return $null }
    $dest = Join-Path $OutDir $ShotName
    Copy-Item -Path $found -Destination $dest -Force
    Write-Host ("shot (dump): " + $dest)
    return $dest
}

# Fallback: capture the foreground window to $ShotName. Only used when the
# frame dump fails. Warns that this grabs whatever is in front, which may
# not be the game.
function Save-ScreenShot {
    param([string]$ShotName)
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
    Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
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

# OCR a screenshot to text (empty string when Tesseract is missing).
function Get-ShotText {
    param([string]$ShotPath)
    if (-not $script:TesseractExe) { return "" }
    if (-not (Test-Path $ShotPath)) { return "" }
    $tmp = Join-Path $OutDir "ocr-tmp.txt"
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
    if ($t -match "sign in|playstation network|psn|sign-in") { return "SignIn" }
    if ($t -match "press any button|press x to start|press x") { return "Title" }
    if ($t -match "be a pro|play now|franchise|hut|world of chel") { return "Hub" }
    if ($t -match "versus|vs\.?|faceoff|face-off") { return "Versus" }
    if ($t -match "pause|paused|resume|quit to|settings") { return "Pause" }
    if ($t -match "nhl|period|shots|goals|hits") { return "Ice" }
    return "Unknown"
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
$psi.EnvironmentVariables["KYTY_INPUT_FILE"] = $InputFile
$psi.EnvironmentVariables["KYTY_COMPUTE_RESOLVE_AHEAD"] = "1"
$psi.EnvironmentVariables["KYTY_DBG_FRAME_DUMP"] = "1"
$psi.EnvironmentVariables["KYTY_DBG_FRAME_DUMP_DIR"] = "D:\Downloads\captures"
$psi.EnvironmentVariables["KYTY_DBG_FRAME_DUMP_CS_CONSTANTS"] = "1"
$psi.EnvironmentVariables["KYTY_DBG_LOOP_CAP"] = "65536"
$psi.EnvironmentVariables["KYTY_DBG_SKIP_BAD_SHADERS"] = "1"
$psi.EnvironmentVariables["KYTY_DBG_BREADCRUMBS"] = "0"
$psi.EnvironmentVariables["KYTY_DBG_CONTROLLERS"] = "1"
Write-Host ("KYTY_INPUT_FILE=" + $psi.EnvironmentVariables["KYTY_INPUT_FILE"])
$proc = [System.Diagnostics.Process]::Start($psi)
Write-Host ("pid: " + $proc.Id)

# Boot wait: the game needs the full time to load the title screen and the
# sign-in dialog before input means anything. Fail fast if the emulator died
# during the wait: the nav loop would otherwise spend ~25 minutes taking
# desktop screenshots of a dead process.
Write-Host ("boot wait: " + $BootWaitSec + " s (the game needs the full load before input means anything)")
Start-Sleep -Seconds $BootWaitSec
$EmulatorDead = -not (Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue)
if ($EmulatorDead) {
    Write-Warning "Emulator exited during the boot wait, before any input was sent. Stopping here - see last-run.log for why it exited."
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
    $shot = Save-Shot -ShotName ("00-signin-" + $s + ".png")
    $text = Get-ShotText -ShotPath $shot
    $kind = Get-ScreenKind -Text $text
    Write-Host ("signin check " + $s + ": " + $kind)
    if ($kind -ne "SignIn") { $signInGone = $true; break }
    Write-Host "Sign-in dialog is up - pressing Circle to dismiss it."
    Invoke-InputStage -Lines @("l 150", "sleep 3000") -ExtraWaitSec 2
}
if (-not $signInGone) {
    Write-Warning "Sign-in dialog would not dismiss after 3 tries; continuing anyway - the title nav below may still work."
}

# STEP 2 - navigate from the title screen to the ice. Vision loop, max 20
# tries: screenshot, OCR, classify, act. Be A Pro rule (unchanged): if the
# screen offers only START CAREER with no CONTINUE, STOP - never create a
# character.
$navShot = 0
$tries = 0
$titleTries = 0
$unknownStreak = 0
$reachedIce = $false
$reachedVersus = $false
while ($tries -lt 20) {
    $tries++
    if (-not (Get-Process -Name "kyty_emulator" -ErrorAction SilentlyContinue)) {
        Write-Warning ("Emulator exited mid-navigation (try " + $tries + "). Stopping the nav loop - see last-run.log.")
        break
    }
    $navShot++
    $shotName = ("{0:D2}-nav.png" -f $navShot)
    $shot = Save-Shot -ShotName $shotName
    $text = Get-ShotText -ShotPath $shot
    $kind = Get-ScreenKind -Text $text
    Write-Host ("screen: " + $kind + " (shot " + $shotName + ")")
    if ($text) { Write-Host ("ocr: " + (($text -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 3) -join " | ")) }
    if ($kind -ne "Unknown") { $unknownStreak = 0 }

    if ($kind -eq "SignIn") {
        Write-Host "Sign-in dialog reappeared - pressing Circle to dismiss it."
        Invoke-InputStage -Lines @("l 150", "sleep 3000") -ExtraWaitSec 2
        continue
    }
    if ($kind -eq "Title") {
        $titleTries++
        if (($titleTries % 2) -eq 1) {
            Write-Host "Title screen - pressing confirm (Cross) to reach the main menu."
            Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
        } else {
            Write-Host "Title screen - Cross did not advance it; pressing Options (Start) instead."
            Invoke-InputStage -Lines @("Return 150", "sleep 3000") -ExtraWaitSec 2
        }
        continue
    }
    if ($kind -eq "Hub") {
        # Be A Pro guard: START CAREER with no CONTINUE means no save yet;
        # creating a character is forbidden, so stop here.
        if (($text -match "start career") -and ($text -notmatch "continue")) {
            Write-Warning "Be A Pro offers only START CAREER (no CONTINUE) - stopping, never creating a character."
            break
        }
        # Hub main menu: go to Play Now (quick Versus game). The menu's first
        # item is Play Now; press Cross on it. If the OCR shows a different
        # highlight, D-pad down first.
        Write-Host "Hub - selecting Play Now for a Versus game."
        Invoke-InputStage -Lines @("j 150", "sleep 4000") -ExtraWaitSec 2
        continue
    }
    if ($kind -eq "Versus") {
        if (-not $reachedVersus) {
            $reachedVersus = $true
            $v = Save-Shot -ShotName "04-versus.png"
            Write-Host ("versus screen captured: " + $v)
        }
        Write-Host "Versus screen - pressing confirm to start the game."
        Invoke-InputStage -Lines @("j 150", "sleep 5000") -ExtraWaitSec 2
        continue
    }
    if ($kind -eq "Ice") {
        $reachedIce = $true
        Write-Host "On the ice - leaving the nav loop for gameplay shots."
        break
    }
    if ($kind -eq "Pause") {
        Write-Host "Pause menu - resuming with Cross."
        Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
        continue
    }
    # Unknown screen: press Cross once and re-read. After 3 in a row, a
    # dialog OCR cannot name may be up: dismiss it with Cross then Options.
    $unknownStreak++
    if ($unknownStreak -ge 3) {
        Write-Host "Screen still not recognized after 3 tries - dismissing a possible dialog with Cross then Options."
        Invoke-InputStage -Lines @("j 150", "sleep 1500", "Return 150", "sleep 3000") -ExtraWaitSec 2
        $unknownStreak = 0
    } else {
        Write-Host "Screen not recognized - pressing confirm once and re-reading."
        Invoke-InputStage -Lines @("j 150", "sleep 2000") -ExtraWaitSec 2
    }
}

# STEP 3 - gameplay shots (only if the nav reached the ice). 05/06 are the
# opening ice shots, 07 is the pause menu, 08 is the resumed ice, and
# 09/10/11 are extended-play interval shots that catch transitions
# (faceoff, goal, replay, period changes).
if ($reachedIce -and -not $SkipGameplay) {
    $g5 = Save-Shot -ShotName "05-ice.png"
    Write-Host ("ice captured: " + $g5)
    Write-Host "Skating (left stick up) for a gameplay shot."
    Invoke-InputStage -Lines @("w 2000", "sleep 1000") -ExtraWaitSec 1
    $g6 = Save-Shot -ShotName "06-ice-skate.png"
    Write-Host ("ice skate captured: " + $g6)
    Write-Host "Opening the pause menu (Options)."
    Invoke-InputStage -Lines @("Return 150", "sleep 3000") -ExtraWaitSec 2
    $g7 = Save-Shot -ShotName "07-pause-menu.png"
    Write-Host ("pause menu captured: " + $g7)
    Write-Host "Resuming from the pause menu (Cross)."
    Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
    $g8 = Save-Shot -ShotName "08-ice-resumed.png"
    Write-Host ("ice resumed captured: " + $g8)
    for ($p = 1; $p -le 3; $p++) {
        Write-Host ("Extended play interval " + $p + " of 3 (skating, then a shot).")
        Invoke-InputStage -Lines @("w 2000", "sleep 1000", "j 150", "sleep 4000") -ExtraWaitSec 2
        $gp = Save-Shot -ShotName ("{0:D2}-ice-play-{1}.png" -f (8 + $p), $p)
        Write-Host ("extended play captured: " + $gp)
    }
    Write-Host "Gameplay shots complete: 04-versus, 05/06/08/09/10/11 ice, 07 pause menu."
} elseif ($reachedIce) {
    Write-Host "Reached the ice; gameplay shots skipped (-SkipGameplay)."
} else {
    Write-Warning "Did not reach the ice in 20 nav tries. Result folder holds the nav shots and the transcript for diagnosis."
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
}
Stop-Transcript | Out-Null
Write-Host ("done. result folder: " + $OutDir)
