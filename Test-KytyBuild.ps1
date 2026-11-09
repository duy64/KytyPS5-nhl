# Test-KytyBuild.ps1
# Automated KyTy NHL 27 build test: versus screen + actual gameplay.
# VISION-GUIDED (v2): the script reads each screenshot with OCR (Tesseract) and
# navigates by what it actually sees, instead of pressing a fixed blind sequence.
# If it lands on the wrong screen (e.g. New Career instead of Continue) it backs
# out instead of continuing blindly. If there is no Continue (no existing career)
# it stops and says so instead of creating a new character.
#
# Uses the emulator's built-in scripted input (KYTY_INPUT_FILE) - no window-focus tricks.
# Screenshots come from the emulator's frame dump (rendered image), so the user can
# keep using the PC while it runs. Do NOT minimize the game window (pauses rendering).
#
# Requires: Windows PowerShell 5.1+ (the user runs PS 5.1), a KyTy build folder, NHL 27 game path.
# Tesseract OCR is auto-installed on first run (UB Mannheim installer, silent) if missing.
# Run:  .\Test-KytyBuild.ps1 -BuildDir "D:\path\to\build" -GamePath "D:\path\to\NHL27"
#   or: .\Test-KytyBuild.ps1 -ReleaseTag "KyTyPS5-2026-11-08-0ec0592-f2dump" -GamePath "D:\path\to\NHL27"
#
# Outputs (in -OutDir): screenshots 01..11 (hub, versus, ice, pause menu, extended
# gameplay + transition catch) + last-run.log copy.
# The emulator is closed at the end unless -KeepRunning is given.

param(
    [string]$BuildDir = "",
    [string]$GamePath = "",
    [string]$OutDir = "",
    [string]$ReleaseTag = "",
    [int]$BootWaitSec = 90,
    [int]$LoadWaitSec = 60,
    [int]$ScreenWidth = 1280,
    [int]$ScreenHeight = 720,
    [switch]$KeepRunning,
    [switch]$SkipGameplay,
    [switch]$NoOcr
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------- settings
# Build: auto-detects the newest KyTyPS5 build folder under D:\Downloads
# (D:\Downloads\<build-name>, e.g. KytyPS5-2026-11-08-0ec0592-f2dump-Windows-x64).
# Pass -BuildDir to test a specific folder, or -ReleaseTag to download one first.
# Game: auto-detects NHL 27 (PPSA34063) from the launcher's game folders
# (kyty.json "game_dirs"). Pass -GamePath to override.
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
function Find-Nhl27Game {
    param([string]$BuildDirPath)
    # kyty.json can live in the shared config root, or next to the emulator
    # (portable mode). Collect every game_dirs root we can find.
    $cfgPaths = @()
    $cfgPaths += (Join-Path $env:LOCALAPPDATA "KytyPS5\configs\kyty.json")
    if ($BuildDirPath) {
        $cfgPaths += (Join-Path $BuildDirPath "kyty.json")
        $cfgPaths += (Join-Path $BuildDirPath "configs\kyty.json")
    }
    $roots = @()
    foreach ($cfg in $cfgPaths) {
        if (-not (Test-Path $cfg)) { continue }
        try { $json = Get-Content $cfg -Raw | ConvertFrom-Json } catch { continue }
        if ($json.game_dirs) { $roots += @($json.game_dirs) }
    }
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($zar in Get-ChildItem -Path $root -Filter "*PPSA34063*.zar" -ErrorAction SilentlyContinue) {
            return $zar.FullName
        }
        $candidates = @()
        if (Test-Path (Join-Path $root "eboot.bin")) { $candidates += $root }
        foreach ($sub in Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue) {
            $candidates += $sub.FullName
        }
        foreach ($cand in $candidates) {
            $param = Join-Path $cand "sce_sys\param.json"
            if ((Test-Path (Join-Path $cand "eboot.bin")) -and (Test-Path $param)) {
                try {
                    $p = Get-Content $param -Raw | ConvertFrom-Json
                    if ($p.titleId -eq "PPSA34063") { return $cand }
                } catch { }
            }
            if ($cand -like "*PPSA34063*") { return $cand }
        }
    }
    return ""
}
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Find-LatestBuild }
function Test-ValidGamePath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    if (-not (Test-Path $Path)) { return $false }
    if ($Path -like "*.zar") { return $true }
    return (Test-Path (Join-Path $Path "eboot.bin"))
}
if ([string]::IsNullOrWhiteSpace($GamePath)) { $GamePath = Find-Nhl27Game -BuildDirPath $BuildDir }
# Saved game path: asked once, remembered next to the script.
$ScriptDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ScriptDir)) { $ScriptDir = (Get-Location).Path }
$GamePathFile = Join-Path $ScriptDir "Test-KytyBuild.game.txt"
if ([string]::IsNullOrWhiteSpace($GamePath) -and (Test-Path $GamePathFile)) {
    $GamePath = (Get-Content $GamePathFile -Raw).Trim()
}
# A build folder is NOT a game folder. If the saved/auto-detected path is not
# a real game (no eboot.bin, not a .zar), discard it and ask again.
if ((-not [string]::IsNullOrWhiteSpace($GamePath)) -and (-not (Test-ValidGamePath -Path $GamePath))) {
    Write-Host ("Ignoring saved path (not a game folder - no eboot.bin): " + $GamePath)
    if (Test-Path $GamePathFile) { Remove-Item $GamePathFile -Force }
    $GamePath = ""
}
while (-not (Test-ValidGamePath -Path $GamePath)) {
    Write-Host "Could not find NHL 27 (PPSA34063) automatically."
    Write-Host "Enter the NHL 27 GAME folder (the one with eboot.bin inside it) or the .zar file."
    Write-Host "That is NOT the KytyPS5 build folder (the one with kyty_emulator.exe) - the script finds that itself."
    Write-Host "Find it in the launcher: Games tab, NHL 27, right-click / properties shows the path."
    $answer = Read-Host "NHL 27 game path (leave empty to quit)"
    $GamePath = $answer.Trim('"').Trim()
    if ([string]::IsNullOrWhiteSpace($GamePath)) { throw "No game path given. Run again and enter the NHL 27 game folder." }
    if (-not (Test-ValidGamePath -Path $GamePath)) {
        Write-Host ("Not a valid game path (no eboot.bin there, and not a .zar): " + $GamePath)
        $GamePath = ""
    }
}
Set-Content -Path $GamePathFile -Value $GamePath -Encoding ASCII
Write-Host ("game path saved to " + $GamePathFile + " (delete that file to be asked again)")
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $stamp = Get-Date -Format "yyyy-MM-dd-HHmm"
    $OutDir = Join-Path ([Environment]::GetFolderPath("Desktop")) ("KyTyTest-" + $stamp)
}

$EmulatorExe = Join-Path $BuildDir "kyty_emulator.exe"
if ([string]::IsNullOrWhiteSpace($BuildDir)) { throw "No KyTy build found in D:\Downloads (check -BuildDir)" }
if (-not (Test-Path $EmulatorExe)) { throw "Emulator not found: $EmulatorExe (check -BuildDir)" }
if ([string]::IsNullOrWhiteSpace($GamePath)) { throw "No game path given and none found. Run again and enter the NHL 27 path when asked." }
if (-not (Test-Path $GamePath)) { throw "Game path not found: $GamePath (check -GamePath)" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Scripted-input file the emulator watches (KYTY_INPUT_FILE). The emulator reads
# it, executes the keys, then DELETES it. We write one stage file at a time.
$InputFile = Join-Path $OutDir "kyty-input.txt"
if (Test-Path $InputFile) { Remove-Item $InputFile -Force }

# ---------------------------------------------------------------- env vars
# Per-game env, matching the launcher's NHL 27 env table. These are set in this
# process only and inherited by the emulator. Add/remove rows here to match yours.
$env:KYTY_COMPUTE_RESOLVE_AHEAD = "1"
$env:KYTY_DBG_FRAME_DUMP = "1"
$env:KYTY_DBG_FRAME_DUMP_DIR = "D:\Downloads\captures"
$env:KYTY_DBG_FRAME_DUMP_CS_CONSTANTS = "1"
$env:KYTY_DBG_LOOP_CAP = "65536"
$env:KYTY_DBG_SKIP_BAD_SHADERS = "1"
$env:KYTY_DBG_BREADCRUMBS = "0"
$env:KYTY_INPUT_FILE = $InputFile

# ---------------------------------------------------------------- helpers
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$CaptureDir = "D:\Downloads\captures"
$TriggerFile = Join-Path $env:LOCALAPPDATA "KytyPS5\run\framedump.trigger"

function Get-LatestFrameDir {
    if (-not (Test-Path $CaptureDir)) { return $null }
    $best = $null; $bestNum = -1
    foreach ($d in Get-ChildItem -Path $CaptureDir -Directory -Filter "frame_*" -ErrorAction SilentlyContinue) {
        $n = 0
        if ([int]::TryParse(($d.Name -replace "frame_", ""), [ref]$n)) {
            if ($n -gt $bestNum) { $bestNum = $n; $best = $d.FullName }
        }
    }
    return $best
}

# Screenshot via the emulator's own frame dump (the rendered image). This works
# even if another window covers the game, so the user can keep using the PC.
# Picks the final rt0 (largest area, then highest seq), preferring the _exp
# (auto-exposed + sRGB) version. Returns the saved path, or $null on failure.
function Save-FrameDumpShot {
    param([string]$Name)
    $before = Get-LatestFrameDir
    $beforeNum = -1
    if ($before) { [int]::TryParse(((Split-Path $before -Leaf) -replace "frame_", ""), [ref]$beforeNum) | Out-Null }
    $trigDir = Split-Path $TriggerFile -Parent
    if (-not (Test-Path $trigDir)) { New-Item -ItemType Directory -Force -Path $trigDir | Out-Null }
    New-Item -ItemType File -Force -Path $TriggerFile | Out-Null
    # The emulator looks for a RELATIVE "framedump.trigger" in its own working
    # directory, which is the build folder when the test launches it (the run
    # folder only applies when the launcher starts it). Create the trigger in
    # the build folder too, or the emulator never sees it after the first dump.
    $buildTrig = Join-Path $BuildDir "framedump.trigger"
    New-Item -ItemType File -Force -Path $buildTrig | Out-Null
    # Wait for a new frame folder (trigger is checked every 30 flips).
    $deadline = (Get-Date).AddSeconds(60)
    $frameDir = $null
    while ((Get-Date) -lt $deadline) {
        $frameDir = Get-LatestFrameDir
        if ($frameDir) {
            $num = -1
            [int]::TryParse(((Split-Path $frameDir -Leaf) -replace "frame_", ""), [ref]$num) | Out-Null
            if ($num -gt $beforeNum) { break }
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $frameDir) { return $null }
    # Timeout with only a stale frame: the trigger produced nothing new.
    # Fail loudly (return null) instead of reusing the stale frame - reusing
    # it copies an old (often black boot) frame 20 times and OCR reads
    # "Unknown" forever. The caller falls back to a screen capture.
    $foundNum = -1
    [int]::TryParse(((Split-Path $frameDir -Leaf) -replace "frame_", ""), [ref]$foundNum) | Out-Null
    if ($foundNum -le $beforeNum) {
        Write-Warning ("Frame dump timeout: no new frame after trigger (latest is still frame_" + $foundNum + ").")
        return $null
    }
    # Wait for the rt0 PNGs to land (async writer), then pick the final one.
    $best = $null; $bestArea = -1; $bestSeq = -1
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        $rt0s = Get-ChildItem -Path $frameDir -Filter "*_rt0_*_exp.png" -ErrorAction SilentlyContinue
        if (-not $rt0s) {
            $rt0s = Get-ChildItem -Path $frameDir -Filter "*_rt0_*.png" -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -notlike "*_exp.png" }
        }
        foreach ($f in $rt0s) {
            $parts = $f.BaseName -split "_"
            if ($parts.Count -lt 4) { continue }
            $seq = 0; [int]::TryParse($parts[0], [ref]$seq) | Out-Null
            $wh = $parts[3] -split "x"
            if ($wh.Count -ne 2) { continue }
            $w = 0; $h = 0
            [int]::TryParse($wh[0], [ref]$w) | Out-Null
            [int]::TryParse($wh[1], [ref]$h) | Out-Null
            $area = $w * $h
            if (($area -gt $bestArea) -or (($area -eq $bestArea) -and ($seq -gt $bestSeq))) {
                $bestArea = $area; $bestSeq = $seq; $best = $f
            }
        }
        if ($best) { Start-Sleep -Seconds 1; break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $best) { return $null }
    $dest = Join-Path $OutDir ($Name + ".png")
    Copy-Item $best.FullName $dest -Force
    Write-Host ("shot (frame dump): " + $dest)
    return $dest
}

function Save-Screenshot {
    param([string]$Name)
    # Prefer the frame dump (rendered image, works even if the game window is
    # covered). Fall back to a screen capture if the dump produced nothing.
    $shot = Save-FrameDumpShot -Name $Name
    if ($shot) { return $shot }
    Write-Warning "Frame dump shot failed; falling back to screen capture."
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $gfx.Dispose()
    $path = Join-Path $OutDir ($Name + ".png")
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("shot (screen): " + $path)
    return $path
}

# Write one input stage file, wait until the emulator consumes it (deletes it),
# then wait out the stage's own durations so the keys finish before the next stage.
function Invoke-InputStage {
    param([string[]]$Lines, [int]$ExtraWaitSec = 0)
    $totalMs = 0
    foreach ($line in $Lines) {
        $parts = $line -split "\s+"
        $dur = 150
        if ($parts.Count -ge 2) { [int]::TryParse($parts[1], [ref]$dur) | Out-Null }
        $totalMs += $dur
    }
    Set-Content -Path $InputFile -Value $Lines -Encoding ASCII
    Write-Host ("input stage: " + ($Lines -join " | "))
    # Wait for the emulator to read + delete the file (poll up to 30 s).
    $deadline = (Get-Date).AddSeconds(30)
    while ((Test-Path $InputFile) -and ((Get-Date) -lt $deadline)) { Start-Sleep -Milliseconds 200 }
    if (Test-Path $InputFile) { Write-Warning "Input file was not consumed; the emulator may not have KYTY_INPUT_FILE set." }
    # Wait for the keys themselves to finish, plus any extra settle time.
    $waitSec = [Math]::Ceiling($totalMs / 1000) + $ExtraWaitSec + 1
    Start-Sleep -Seconds $waitSec
}

function Wait-ForProcess {
    param([string]$Name, [int]$TimeoutSec = 30)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Get-Process -Name $Name -ErrorAction SilentlyContinue) { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

# ---------------------------------------------------------------- OCR (vision)
# Tesseract reads the menu text off each screenshot so the script navigates by
# what it actually sees. Verified on real game screenshots: it reads
# "BE A PRO CAREER", "START CAREER", "NEW CAREER", "FIRST NAME", "PLAY NOW".
# If Tesseract is missing it is installed once (UB Mannheim, silent). If OCR
# is unavailable the script falls back to the blind sequence (-NoOcr forces that).
$script:TesseractExe = $null
function Get-TesseractExe {
    if ($script:TesseractExe) { return $script:TesseractExe }
    $candidates = @()
    $cmd = Get-Command "tesseract" -ErrorAction SilentlyContinue
    if ($cmd) { $candidates += $cmd.Source }
    $candidates += "C:\Program Files\Tesseract-OCR\tesseract.exe"
    $candidates += "C:\Program Files (x86)\Tesseract-OCR\tesseract.exe"
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { $script:TesseractExe = $c; return $c }
    }
    # Not found: download the UB Mannheim installer and run it silently (one time).
    # Needs admin (UAC prompt once). Installs to C:\Program Files\Tesseract-OCR.
    $url = "https://github.com/tesseract-ocr/tesseract/releases/download/5.5.3/tesseract-ocr-w64-setup-5.5.3.20260724.exe"
    $installer = Join-Path $env:TEMP "tesseract-setup.exe"
    try {
        Write-Host "Tesseract not found - downloading installer (one time)..."
        Invoke-WebRequest -Uri $url -OutFile $installer -UseBasicParsing
        Write-Host "Installing Tesseract silently (UAC prompt once)..."
        $p = Start-Process -FilePath $installer -ArgumentList "/VERYSILENT", "/NORESTART" -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Warning ("Tesseract installer exit code " + $p.ExitCode); return $null }
        Start-Sleep -Seconds 3
        if (Test-Path "C:\Program Files\Tesseract-OCR\tesseract.exe") {
            $script:TesseractExe = "C:\Program Files\Tesseract-OCR\tesseract.exe"
            return $script:TesseractExe
        }
        return $null
    }
    catch {
        Write-Warning ("Tesseract install failed: " + $_.Exception.Message)
        return $null
    }
}

# Run OCR on an image, return the recognized text (may be empty on failure).
# Default page segmentation (PSM 3) read the game menus best in testing.
function Get-OcrText {
    param([string]$ImagePath)
    $exe = $script:TesseractExe
    if (-not $exe) { return "" }
    if (-not (Test-Path $ImagePath)) { return "" }
    try {
        $out = & $exe $ImagePath stdout 2>$null
        if ($out) { return ($out -join "`n") }
        return ""
    }
    catch { return "" }
}

# Identify which game screen the OCR text came from. Matching is deliberately
# loose: OCR misreads letters (e.g. "BE APRO CAREER", "WORLD OF GHEL"), so we
# match short distinctive fragments and normalize spaces out for the key ones.
function Get-ScreenName {
    param([string]$OcrText)
    if ([string]::IsNullOrWhiteSpace($OcrText)) { return "Unknown" }
    $flat = ($OcrText.ToUpper() -replace "\s+", " ")
    $nospace = ($OcrText.ToUpper() -replace "[^A-Z0-9]", "")
    # Character creation: the wrong place. Back out immediately.
    if (($flat -match "NEW CAREER") -and ($flat -match "FIRST NAME")) { return "NewCareer" }
    if ($flat -match "PLEASE ENTER A FIRST NAME") { return "NewCareer" }
    # Be A Pro menu WITH an existing career to resume.
    if ($flat -match "CONTINUE") { return "BeAProContinue" }
    # Be A Pro menu with NO existing career (only Start Career). Stop, do not create one.
    if (($flat -match "START CAREER") -and (($flat -match "BE A PRO") -or ($nospace -match "BEAPRO"))) { return "BeAProNoSave" }
    # Hub: the Be A Pro career home screen (Play Next Game is the way to versus).
    if (($flat -match "PLAY NEXT GAME") -or ($flat -match "NEXT GAME")) { return "Hub" }
    # In-game pause menu.
    if ($flat -match "PAUSED") { return "Pause" }
    if (($flat -match "PAUSE") -and (($flat -match "RESUME") -or ($flat -match "QUIT") -or ($flat -match "SETTINGS"))) { return "Pause" }
    # PSN sign-in dialog (EA servers). Dismiss with OK; the game plays offline.
    if ($flat -match "SIGN IN REQUIRED") { return "SignIn" }
    # Main menu.
    if (($flat -match "PLAY NOW") -and (($flat -match "BE A PRO") -or ($nospace -match "BEAPRO"))) { return "MainMenu" }
    # Title / press-start screen.
    if (($flat -match "PRESS") -and (($flat -match "START") -or ($flat -match "ENTER"))) { return "Title" }
    if ($flat -match "PRESS ANY BUTTON") { return "Title" }
    return "Unknown"
}

# Take a shot, OCR it, and report which screen we are on.
# Returns a hashtable: Screen, Shot (path or $null), Text.
function Get-CurrentScreen {
    param([string]$ShotName)
    $shot = Save-Screenshot -Name $ShotName
    $text = ""
    if ($shot -and $script:TesseractExe) { $text = Get-OcrText -ImagePath $shot }
    $screen = Get-ScreenName -OcrText $text
    Write-Host ("screen: " + $screen + " (shot " + $ShotName + ")")
    if ($text) {
        $oneline = (($text -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ }) -join " | ")
        if ($oneline) { Write-Host ("ocr: " + $oneline) }
    }
    return @{ Screen = $screen; Shot = $shot; Text = $text }
}

# ---------------------------------------------------------------- download (optional)
if ($ReleaseTag) {
    $zipName = $ReleaseTag + "-Windows-x64.zip"
    $zipUrl = "https://github.com/duy64/KytyPS5-nhl/releases/download/" + $ReleaseTag + "/" + $zipName
    $zipPath = Join-Path $OutDir $zipName
    Write-Host ("downloading: " + $zipUrl)
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath
    $extractDir = Join-Path $OutDir ($ReleaseTag)
    Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force
    $found = Get-ChildItem -Path $extractDir -Recurse -Filter "kyty_emulator.exe" | Select-Object -First 1
    if (-not $found) { throw "kyty_emulator.exe not found in downloaded release." }
    $BuildDir = $found.DirectoryName
    $EmulatorExe = $found.FullName
    Write-Host ("build extracted to: " + $BuildDir)
}

# ---------------------------------------------------------------- saves
# The emulator resolves its save data from a relative "_SaveData" folder, so it
# looks next to the build (the working directory), not in %LOCALAPPDATA%.
# Copy the real saves there first so the test build sees the existing career
# (Continue) instead of only "Start Career". This copies TO the build folder
# only; the originals in %LOCALAPPDATA% are never modified by the test.
$SaveSrc = Join-Path $env:LOCALAPPDATA "KytyPS5\run\_SaveData"
$SaveDst = Join-Path $BuildDir "_SaveData"
if (Test-Path $SaveSrc) {
    if (-not (Test-Path $SaveDst)) { New-Item -ItemType Directory -Force -Path $SaveDst | Out-Null }
    Copy-Item (Join-Path $SaveSrc "*") $SaveDst -Recurse -Force
    Write-Host ("saves copied: " + $SaveSrc + " -> " + $SaveDst)
}
else {
    Write-Host ("WARNING: no save data found at " + $SaveSrc + " (test will show Start Career only)")
}

# ---------------------------------------------------------------- OCR setup
if (-not $NoOcr) {
    $script:TesseractExe = Get-TesseractExe
    if ($script:TesseractExe) { Write-Host ("OCR ready: " + $script:TesseractExe) }
    else { Write-Warning "OCR unavailable - falling back to blind sequence. Run with -NoOcr to skip this warning." }
}
else {
    Write-Host "OCR disabled (-NoOcr) - blind sequence."
}

# ---------------------------------------------------------------- launch
$buildTag = Split-Path $BuildDir -Leaf
Write-Host ("build: " + $buildTag)
Write-Host ("game:  " + $GamePath)
Write-Host ("out:   " + $OutDir)

$emuArgs = @("--game", $GamePath, "--redzone", "--screen-width", $ScreenWidth, "--screen-height", $ScreenHeight)
$argString = '--game "' + $GamePath + '" --redzone --screen-width ' + $ScreenWidth + ' --screen-height ' + $ScreenHeight
Write-Host ("launch: " + $EmulatorExe + " " + $argString)
Write-Host ("screen: " + $ScreenWidth + "x" + $ScreenHeight + " (low res for speed; final confirmation runs at full res)")
# Launch with an explicit environment block so KYTY_INPUT_FILE (and the other
# KYTY_* rows) definitely reach the emulator. Start-Process inheritance was
# not delivering KYTY_INPUT_FILE (log showed zero InputFile lines even though
# the binary contains the feature), so set the variables directly on the
# child process instead of relying on inheritance.
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
Write-Host ("KYTY_INPUT_FILE=" + $psi.EnvironmentVariables["KYTY_INPUT_FILE"])
$proc = [System.Diagnostics.Process]::Start($psi)
if (-not (Wait-ForProcess -Name "kyty_emulator" -TimeoutSec 30)) { throw "Emulator did not start." }
Write-Host ("boot wait: " + $BootWaitSec + " s (first run of a new build compiles shaders - slow)")
Start-Sleep -Seconds $BootWaitSec

# ---------------------------------------------------------------- navigation (vision-guided)
# Key names are SDL key names. Buttons (default KyTy keyboard map):
#   j = Cross (confirm) | l = Circle (back) | i = Triangle | k = Square
#   Up/Down/Left/Right = D-pad | w/a/s/d = left stick | Return = Options
# The loop reads the screen after every move and decides the next move from
# what it sees. It never creates a new character: if Be A Pro shows only
# "Start Career" (no Continue) it stops and reports it.
$shotNum = 0
function New-ShotName {
    $script:shotNum++
    return ("{0:D2}-nav" -f $script:shotNum)
}

# Blind fallback sequence (used only when OCR is unavailable): the old fixed
# stages, kept so the test still runs without Tesseract.
function Invoke-BlindNavigation {
    Write-Host "== blind stage 1: title -> main menu =="
    Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
    Save-Screenshot -Name "01-after-title"
    Write-Host "== blind stage 2: main menu -> Be A Pro =="
    Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
    Save-Screenshot -Name "02-be-a-pro-menu"
    Write-Host "== blind stage 3: Be A Pro -> Continue -> hub =="
    Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 10
    Save-Screenshot -Name "03-hub"
    Write-Host "== blind stage 4: hub -> Play Next Game -> versus screen =="
    Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 5
    Save-Screenshot -Name "04-versus"
}

if ((-not $script:TesseractExe) -or $NoOcr) {
    Invoke-BlindNavigation
}
else {
    # Vision-guided: keep reading the screen and acting on it until we reach
    # the hub, or we hit a stop condition (no save) or run out of tries.
    $reachedHub = $false
    $noSave = $false
    $tries = 0
    $maxTries = 20
    while ((-not $reachedHub) -and (-not $noSave) -and ($tries -lt $maxTries)) {
        $tries++
        $cur = Get-CurrentScreen -ShotName (New-ShotName)
        $screen = $cur.Screen
        if ($screen -eq "Title") {
            Write-Host "Title screen - pressing confirm to reach the main menu."
            Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
        }
        elseif ($screen -eq "SignIn") {
            # PSN sign-in dialog. OK is the only button; confirm dismisses it
            # and the game continues offline. If it comes back, dismiss again.
            Write-Host "Sign-in dialog - pressing OK to dismiss and continue offline."
            Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
        }
        elseif ($screen -eq "MainMenu") {
            # Be A Pro Career is the top item and is highlighted on entry, so
            # confirm selects it. If that ever changes, the next read will show
            # we did not land on Be A Pro and we back out below.
            Write-Host "Main menu - selecting Be A Pro Career."
            Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 2
        }
        elseif ($screen -eq "BeAProContinue") {
            Write-Host "Be A Pro menu - Continue found, resuming the career."
            Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 5
        }
        elseif ($screen -eq "BeAProNoSave") {
            Write-Host "STOP: Be A Pro shows only Start Career - no existing career found. Not creating a new one."
            $noSave = $true
        }
        elseif ($screen -eq "NewCareer") {
            Write-Host "Wrong screen (character creation) - pressing back to leave it."
            Invoke-InputStage -Lines @("l 150", "sleep 2000") -ExtraWaitSec 2
        }
        elseif ($screen -eq "Hub") {
            Write-Host "Hub reached."
            $reachedHub = $true
        }
        elseif ($screen -eq "Unknown") {
            Write-Host "Screen not recognized - pressing confirm once and re-reading."
            Invoke-InputStage -Lines @("j 150", "sleep 2000") -ExtraWaitSec 2
        }
        else {
            Write-Host ("Unexpected screen '" + $screen + "' - pressing back and re-reading.")
            Invoke-InputStage -Lines @("l 150", "sleep 2000") -ExtraWaitSec 2
        }
    }
    if ($noSave) {
        Write-Host "No existing Be A Pro career was found. Stopping before the versus/gameplay stages."
    }
    elseif (-not $reachedHub) {
        Write-Warning ("Did not reach the hub after " + $tries + " tries. Check the nav screenshots.")
    }
}

# Hub shot (the "03-hub" deliverable) if we got there.
if ($reachedHub -or (-not $script:TesseractExe) -or $NoOcr) {
    if ($reachedHub) {
        $hub = Get-CurrentScreen -ShotName "03-hub"
        if ($hub.Screen -ne "Hub") { Write-Warning ("Expected hub, but screen reads '" + $hub.Screen + "'. Continuing anyway.") }
    }
    # Versus: from the hub, Play Next Game is the confirm action.
    Write-Host "== hub -> Play Next Game -> versus screen =="
    Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 3
    Save-Screenshot -Name "04-versus"

    if (-not $SkipGameplay) {
        Write-Host "== versus -> start game -> ice =="
        Invoke-InputStage -Lines @("j 150", "sleep 3000") -ExtraWaitSec 0
        Write-Host ("game load wait: " + $LoadWaitSec + " s")
        Start-Sleep -Seconds $LoadWaitSec
        Save-Screenshot -Name "05-ice"

        Write-Host "== skate forward, then screenshot =="
        # Hold W (left stick up) to skate; a short burst so it stays in frame.
        Invoke-InputStage -Lines @("w 2500", "sleep 1000", "a 800", "sleep 500", "w 1500") -ExtraWaitSec 2
        Save-Screenshot -Name "06-ice-skate"

        # In-game menus: pause (Options), screenshot the pause menu, resume.
        Write-Host "== in-game pause menu =="
        Invoke-InputStage -Lines @("Return 150", "sleep 2500") -ExtraWaitSec 2
        $pauseShot = Save-Screenshot -Name "07-pause-menu"
        if ($pauseShot -and $script:TesseractExe) {
            $pauseText = Get-OcrText -ImagePath $pauseShot
            $pauseScreen = Get-ScreenName -OcrText $pauseText
            Write-Host ("pause screen reads: " + $pauseScreen)
        }
        # Resume: Circle backs out of pause; if that does not resume, confirm does.
        Invoke-InputStage -Lines @("l 150", "sleep 2000") -ExtraWaitSec 2
        Save-Screenshot -Name "08-ice-resumed"

        # Extended on-ice gameplay + transition catch: skate and shoot at
        # intervals so a faceoff, goal, replay or period transition that fires
        # during play is captured. These are wall-clock captures, not scripted
        # events, so a quiet stretch just yields more clean ice shots.
        Write-Host "== extended gameplay: skate + interval shots (catches transitions) =="
        Invoke-InputStage -Lines @("w 3000", "sleep 500", "d 800", "sleep 500", "w 2000") -ExtraWaitSec 2
        Save-Screenshot -Name "09-ice-play-1"
        Start-Sleep -Seconds 10
        Invoke-InputStage -Lines @("w 2000", "sleep 500", "a 800", "sleep 500", "s 1200") -ExtraWaitSec 2
        Save-Screenshot -Name "10-ice-play-2"
        Start-Sleep -Seconds 10
        Invoke-InputStage -Lines @("w 2000", "sleep 500") -ExtraWaitSec 1
        Save-Screenshot -Name "11-ice-play-3"
    }
}

# ---------------------------------------------------------------- collect + close
$logSrc = Join-Path $env:LOCALAPPDATA "KytyPS5\logs\last-run.log"
if (Test-Path $logSrc) {
    Copy-Item $logSrc (Join-Path $OutDir "last-run.log") -Force
    Write-Host "log saved"
}

if (-not $KeepRunning) {
    Write-Host "closing emulator"
    try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
    Start-Sleep -Seconds 3
    if (Test-Path $logSrc) { Copy-Item $logSrc (Join-Path $OutDir "last-run.log") -Force }
}

Write-Host ("done. Results in: " + $OutDir)
Write-Host "Send the 01..11 screenshots (and last-run.log if anything looked wrong) for review."

# ---------------------------------------------------------------- cleanup raw frame dumps
# The screenshots are already copied into $OutDir; the raw frame_N folders in
# the captures dir (several PNGs per frame) are no longer needed and pile up
# fast. Delete them so a test leaves no leftover bulk behind. Only frame_*
# folders are removed; anything else in the captures dir is left alone.
$frameDirs = Get-ChildItem -Path $CaptureDir -Directory -Filter "frame_*" -ErrorAction SilentlyContinue
if ($frameDirs) {
    $count = ($frameDirs | Measure-Object).Count
    Write-Host ("cleaning up " + $count + " raw frame dump folders in " + $CaptureDir)
    foreach ($d in $frameDirs) {
        try { Remove-Item $d.FullName -Recurse -Force -ErrorAction Stop } catch { Write-Warning ("could not remove " + $d.FullName + ": " + $_.Exception.Message) }
    }
}
