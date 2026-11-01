# Flash helper: owns the board's USB port and talks to the agent through plain files in <repo>\.devloop
# (docs/PROTOCOL.md section 1). Two ways to run it: in its own window (start_flash_helper.bat; close the window to stop
# it), or windowless in the background, started by Claude Code with run_in_background so nothing opens on the user's
# screen (stop it by stopping that task; Q / Esc need a window, use stop.request).
#   flash.request   (body: log seconds, default 60) flash the parts staged in .devloop\stage, then log the serial port
#   reboot.request  same without flashing: restart through the test console's `reboot` with the port kept open (the
#                   boot log is whole), else esptool's hard reset; then log (re-runs the boot diagnostics)
#   stop.request    end the log window early (or press Q / Esc in the window, when it has one)
# What to flash comes from <build_dir>\flasher_args.json (ESP-IDF writes it), the chip / baud / port from forge.json.
# Only staged copies are flashed (tools/devloop/stage.py): each is checked against stage\manifest.json by md5 first,
# because a rebuilt file pushed to the same path once delivered the previous version.
# esptool: tools\esptool.exe (standalone v4.8.1, git-ignored) if present, else "python -m esptool".
$ErrorActionPreference = "Continue"

function Find-Root {
  $d = $PSScriptRoot
  while ($d) {
    if (Test-Path (Join-Path $d "forge.json")) { return $d }
    $up = Split-Path $d -Parent
    if (-not $up -or $up -eq $d) { break }
    $d = $up
  }
  throw "no forge.json above $PSScriptRoot"
}

$Root = Find-Root
$Cfg = Get-Content (Join-Path $Root "forge.json") -Raw | ConvertFrom-Json
$Dev = Join-Path $Root ".devloop"
New-Item -ItemType Directory -Force $Dev | Out-Null
Set-Location $Dev
# The window title shows the state; windowless (background) there is no window to title
function Title($t) { try { $Host.UI.RawUI.WindowTitle = $t } catch {} }
Title "ESP flash helper - waiting"

function Get-Cfg($name, $default) { if ($null -ne $Cfg.$name -and "$($Cfg.$name)" -ne "") { $Cfg.$name } else { $default } }
$Chip = Get-Cfg "chip" "esp32s3"
$Baud = Get-Cfg "baud" 460800
$CfgPort = Get-Cfg "port" ""
$BuildDir = Join-Path $Root ((Get-Cfg "build_dir" "build") -replace '/', '\')

function Stamp { Get-Date -Format "HH:mm:ss" }
function Say($msg, $color = "Gray") {
  Write-Host "[$(Stamp)] $msg" -ForegroundColor $color
  Add-Content -Path "flash_helper.log" -Value "[$(Get-Date -Format s)] $msg" -Encoding ASCII
}
function Status($s) { Set-Content -Path "flash.status" -Value $s -Encoding ASCII }
function Done($line) { Set-Content -Path "flash.done" -Value $line -Encoding ASCII }
function Md5($path) { (Get-FileHash -Algorithm MD5 -Path $path).Hash.ToLower() }

# esptool: the standalone exe, else the Python module (pip install esptool, or ESP-IDF's environment)
$Esptool = @()
$exe = Join-Path $Root "tools\esptool.exe"
if (Test-Path $exe) { $Esptool = @($exe) }
elseif (Get-Command python -ErrorAction SilentlyContinue) { $Esptool = @("python", "-m", "esptool") }
elseif (Get-Command py -ErrorAction SilentlyContinue) { $Esptool = @("py", "-3", "-m", "esptool") }

# The board's COM port: forge.json's, else the one board on USB (Espressif's VID 303A, or a CP210x bridge, VID 10C4),
# else among several the chip that answers esptool's chip_id as forge.json's chip (an ESP32-P4 beside the S3), kept
# in .devloop\port until it is gone. $null lets esptool look.
function Find-BoardPort {
  if ($CfgPort) { return $CfgPort }
  $cands = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
       Where-Object { ($_.PNPDeviceID -match 'VID_303A' -or $_.PNPDeviceID -match 'VID_10C4') -and $_.Name -match '\((COM\d+)\)' } |
       ForEach-Object { $null = $_.Name -match '\((COM\d+)\)'; $Matches[1] })
  if ($cands.Count -eq 0) { return $null }
  $cache = Join-Path $Root ".devloop\port"
  if (Test-Path $cache) { $p = (Get-Content $cache -Raw).Trim(); if ($cands -contains $p) { return $p } }
  if ($cands.Count -eq 1) { Set-Content $cache $cands[0] -Encoding ASCII; return $cands[0] }
  if ($Esptool.Count -eq 0) { return $cands[0] }
  $pre = if ($Esptool.Count -gt 1) { $Esptool[1..($Esptool.Count - 1)] } else { @() }
  foreach ($c in $cands) {
    Say "Several boards: asking the chip on $c whether it is a $Chip ..."
    $null = & $Esptool[0] @pre --chip $Chip -p $c --before default_reset --after hard_reset chip_id 2>&1
    if ($LASTEXITCODE -eq 0) { Set-Content $cache $c -Encoding ASCII; Say "$Chip on $c (kept in .devloop\port)" "Green"; return $c }
  }
  return $null
}

# Logs the port for $secs s (monitor.ps1); -Reboot restarts through the test console first. Returns its exit code:
# 2 = stopped early, 3 = no restart through the console
function Run-Monitor($port, $secs, [switch]$Reboot) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "monitor.ps1"), "-Port", $port,
         "-Seconds", $secs)
  if ($Reboot) { $a += "-Reboot" }
  & powershell @a | Out-Null
  return $LASTEXITCODE
}

# The esptool arguments for a flash, or a string saying why not (a stale or incomplete stage)
function Flash-Args {
  $mfPath = Join-Path $Dev "stage\manifest.json"
  if (-not (Test-Path $mfPath)) { return "no stage\manifest.json (run tools/devloop/stage.py)" }
  $man = Get-Content $mfPath -Raw | ConvertFrom-Json
  # The build that was staged (stage.py --build build/forge: another folder than forge.json's; checking the staged
  # copies against forge.json's refused them as "changed since it was staged", 2026-10-09)
  $bd = if ($man.build_dir) { Join-Path $Root ($man.build_dir -replace '/', '\') } else { $BuildDir }
  $faPath = Join-Path $bd "flasher_args.json"
  if (-not (Test-Path $faPath)) { return "no $faPath (build the firmware first)" }
  $fa = Get-Content $faPath -Raw | ConvertFrom-Json
  $fs = $fa.flash_settings
  $extra = $fa.extra_esptool_args
  $chip = if ($extra.chip) { $extra.chip } else { $Chip }
  $a = @("--chip", $chip, "-b", "$Baud",
         "--before", $(if ($extra.before) { $extra.before } else { "default_reset" }),
         "--after", $(if ($extra.after) { $extra.after } else { "hard_reset" }))
  if ($extra.stub -eq $false) { $a += "--no-stub" }
  $a += @("write_flash", "--flash_mode", $fs.flash_mode, "--flash_freq", $fs.flash_freq, "--flash_size", $fs.flash_size)
  foreach ($p in $fa.flash_files.PSObject.Properties) {
    $off = $p.Name
    $part = $man.parts | Where-Object { [Convert]::ToInt64($_.offset, 16) -eq [Convert]::ToInt64($off, 16) } |
            Select-Object -First 1
    if (-not $part) { return "$($p.Value) at $off is not staged (layout changed since staging? run stage.py again)" }
    $f = Join-Path $Dev "stage\$($part.file)"
    if (-not (Test-Path $f)) { return "staged file $($part.file) is missing" }
    $got = Md5 $f
    if ($got -ne $part.md5.ToLower()) { return "staged $($part.file): md5 $got, the manifest says $($part.md5)" }
    # A build newer than the stage: flashing would test old firmware
    $src = Join-Path $bd ($p.Value -replace '/', '\')
    if ((Test-Path $src) -and ((Md5 $src) -ne $got)) { return "$($p.Value) changed since it was staged (run stage.py again)" }
    Say ("  {0,9}  {1}  {2:N0} B  md5 ok" -f $off, $part.file, (Get-Item $f).Length)
    $a += @($off, $f)
  }
  return ,$a
}

Say "Flash helper for $Root (chip $Chip, $Baud baud, build $BuildDir)" "Cyan"
if ($Esptool.Count -eq 0) { Say "No esptool: put esptool.exe in tools\ or install Python with esptool" "Red" }
else { Say "esptool: $($Esptool -join ' ')" }
# Requests left from before this start: whoever wrote them has given up waiting (the harness waits ~4 min), and a
# flash nobody watches is a surprise. Drop them (a restarted helper once flashed one left an hour earlier).
foreach ($old in @("flash.request", "reboot.request", "stop.request", "serial.send")) {
  if (Test-Path $old) { Remove-Item $old -Force; Say "Ignored a $old left from before this start" "Yellow" }
}
Say "Waiting for .devloop\flash.request / reboot.request ..." "Cyan"
Status "idle"

while ($true) {
  $req = if (Test-Path "flash.request") { "flash.request" } elseif (Test-Path "reboot.request") { "reboot.request" } else { $null }
  if (-not $req) { Start-Sleep -Seconds 1; continue }
  $reboot = $req -eq "reboot.request"

  # ---- read request ----
  $secs = 60
  $txt = (Get-Content $req -Raw -ErrorAction SilentlyContinue)
  if ($txt -match '^\s*(\d{1,5})\s*$') { $secs = [int]$Matches[1] }
  Remove-Item $req -Force
  Remove-Item "flash.done", "stop.request" -Force -ErrorAction SilentlyContinue
  $start = Get-Date
  Set-Content "flash.running" (Get-Date -Format s)
  Write-Host ""
  if ($reboot) { Say "=== Reboot request received, no flashing (serial log: $secs s) ===" "Cyan" }
  else { Say "=== Flash request received (serial log: $secs s) ===" "Cyan" }
  Title "ESP flash helper - FLASHING"
  Status "flashing"

  # ---- restart through the test console (reboot.request) ----
  # esptool's hard reset re-enumerates the USB and the first ~2.5 s of boot log are lost (L154); a software restart
  # with the port open keeps them (L191). esptool only when no boot follows (no console, a hung board).
  $consoleReboot = $false
  $monExit = 0
  if ($reboot) {
    $port = Find-BoardPort
    if ($port) {
      Say "Restarting through the test console on $port (the port stays open: the boot log is kept from its first line)"
      Title "ESP flash helper - logging serial ($secs s)"
      $monExit = Run-Monitor $port $secs -Reboot
      if ($monExit -ne 3) { $consoleReboot = $true }
      else { Say "No restart through the test console: esptool's hard reset instead" "Yellow"; $monExit = 0 }
    }
  }
  if (-not $consoleReboot) {   # esptool: a flash, or a restart the console did not do (unindented)

  # ---- esptool arguments ----
  $found = Find-BoardPort
  $portArgs = if ($found) { @("-p", $found) } else { @() }
  $why = $null
  if ($Esptool.Count -eq 0) { $why = "no esptool (tools\esptool.exe or python -m esptool)" }
  elseif ($reboot) {
    $esptoolArgs = $portArgs + @("--chip", $Chip, "--before", "default_reset", "--after", "hard_reset", "chip_id")
  } else {
    $fa = Flash-Args
    if ($fa -is [string]) { $why = $fa } else { $esptoolArgs = $portArgs + $fa }
  }
  if ($why) {
    Say "NOT FLASHING: $why" "Red"
    Status "flash_failed"
    Done "exit=3 stage=verify started=$(Get-Date $start -Format s) finished=$(Get-Date -Format s)"
    Remove-Item "flash.running" -Force -ErrorAction SilentlyContinue
    [console]::beep(400, 600)
    Title "ESP flash helper - NOT FLASHED (waiting)"
    continue
  }

  # ---- flash ----
  $lines = New-Object System.Collections.Generic.List[string]
  $cmd = $Esptool[0]
  $pre = if ($Esptool.Count -gt 1) { $Esptool[1..($Esptool.Count - 1)] } else { @() }
  Add-Content -Path "flash_helper.log" -Value "[$(Get-Date -Format s)] esptool $($esptoolArgs -join ' ')" -Encoding ASCII
  & $cmd @pre @esptoolArgs 2>&1 | ForEach-Object {
    $l = "$_"
    $lines.Add($l)
    if ($l -match 'Serial port|Chip is|Writing at .*\(100 ?%\)|Wrote |Hash of data|Hard resetting|error|Error|failed') {
      Write-Host "    $l" -ForegroundColor DarkGray
    }
  }
  $rc = $LASTEXITCODE
  $lines | Set-Content -Path "flash_log.txt" -Encoding ASCII
  $port = ($lines | Select-String -Pattern 'Serial port (COM\d+)' | Select-Object -Last 1).Matches.Groups[1].Value
  if (-not $port) { $port = $CfgPort }
  $flashSecs = [int]((Get-Date) - $start).TotalSeconds

  if ($rc -ne 0) {
    Say "$(if ($reboot) {"REBOOT"} else {"FLASH"}) FAILED (exit $rc) after $flashSecs s - last lines:" "Red"
    $lines | Select-Object -Last 6 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    Say "Tip: if no port was found, hold BOOT, tap RESET, release BOOT, and request again." "Yellow"
    Status "flash_failed"
    Done "exit=$rc port=$port stage=flash started=$(Get-Date $start -Format s) finished=$(Get-Date -Format s)"
    Remove-Item "flash.running" -Force -ErrorAction SilentlyContinue
    [console]::beep(400, 600)
    Title "ESP flash helper - FLASH FAILED (waiting)"
    continue
  }
  Say "$(if ($reboot) {"REBOOT"} else {"FLASH"}) OK on $port in $flashSecs s - board is restarting" "Green"
  # What the tools cached about the board belongs to the old firmware (PROTOCOL.md section 1: reset after every flash)
  if (-not $reboot) { Remove-Item "ip", "key" -Force -ErrorAction SilentlyContinue }
  [console]::beep(1000, 150)

  # ---- serial log ----
  Status "logging"
  Title "ESP flash helper - logging serial ($secs s)"
  Say "Logging serial output for $secs s (saved to .devloop\serial_log.txt). Press Q or Esc to stop early ..."
  $monExit = Run-Monitor $port $secs
  }   # (not a console restart)
  $flashSecs = if ($consoleReboot) { 0 } else { $flashSecs }
  $early = if ($monExit -eq 2) { 1 } else { 0 }
  if ($early) { Say "Serial log stopped early" "Yellow" }

  # ---- summary ----
  $log = @(Get-Content "serial_log.txt" -ErrorAction SilentlyContinue)
  $errs = @($log | Where-Object { $_ -match '^E \(' }).Count
  $warns = @($log | Where-Object { $_ -match '^W \(' }).Count
  # the unexpected ones: not the restart asked for (an esptool reset's own boot is lost, L154)
  $resets = @($log | Where-Object { $_ -match 'rst:0x' }).Count
  if ($consoleReboot -and $resets -gt 0) { $resets-- }
  $color = if ($errs -gt 0 -or $resets -gt 0) { "Yellow" } else { "Green" }
  Say ("Serial log saved: {0} lines, {1} errors, {2} warnings, {3} resets" -f $log.Count, $errs, $warns, $resets) $color
  $total = [int]((Get-Date) - $start).TotalSeconds
  Done "exit=0 port=$port flash_s=$flashSecs total_s=$total errors=$errs warnings=$warns resets=$resets stopped_early=$early started=$(Get-Date $start -Format s) finished=$(Get-Date -Format s)"
  Remove-Item "flash.running" -Force -ErrorAction SilentlyContinue
  Status "idle"
  Say "=== Done in $total s. Waiting for the next request ===" "Cyan"
  [console]::beep(1200, 120); [console]::beep(1500, 120)
  Title "ESP flash helper - waiting"
}
