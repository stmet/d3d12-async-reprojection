<#
.SYNOPSIS
  Analyze the lean reprojection presenter's telemetry CSV (dxgi_telemetry.csv).

.DESCRIPTION
  Splits the log into config SEGMENTS (a run of stat windows sharing the same tunables) so you can
  A/B settings directly: toggle late-warp / drop the lead floor in-game, and each config shows up as
  its own segment with its own latency stats. Crucially it GATES the latency numbers on present
  stability -- input->scanout is only anchored to a real vblank when presents are phase-locked to the
  refresh (low jitter, ~0 missed vblanks). A segment that isn't phase-locked is flagged SUSPECT so we
  never trust a hallucinated number.

.PARAMETER Path
  Path to dxgi_telemetry.csv. Defaults to the deployed game copy.

.EXAMPLE
  ./analyze_telemetry.ps1
  ./analyze_telemetry.ps1 -Path .\dxgi_telemetry.csv
#>
param(
    [string]$Path = "C:\Games\Cyberpunk 2077\bin\x64\dxgi_telemetry.csv"
)

if (-not (Test-Path $Path)) { Write-Error "Telemetry not found: $Path"; exit 1 }

$rows   = Import-Csv -Path $Path
$stats  = @($rows | Where-Object { $_.kind -eq 'STAT' })
$events = @($rows | Where-Object { $_.kind -eq 'EVENT' })

if ($stats.Count -eq 0) { Write-Warning "No STAT rows yet (let the presenter run a few seconds)."; }

function Pct([double[]]$v, [double]$p) {
    if ($v.Count -eq 0) { return 0.0 }
    $s = $v | Sort-Object
    $i = [int][math]::Floor($p / 100.0 * ($s.Count - 1))
    return [double]$s[$i]
}
function Med([double[]]$v) { return (Pct $v 50) }
function ColD($name) { return @($stats | ForEach-Object { [double]$_.$name }) }

# ---- config signature -> segments -------------------------------------------------------------
$sigFields = 'enabled','mode','angular','sens','fov','gain','sign','autotrim','autolead','lead_floor_ms','maxfif','vsync','latewarp'
function Sig($r) { return ($sigFields | ForEach-Object { $r.$_ }) -join '|' }

$segments = @()
$cur = $null
foreach ($r in $stats) {
    $sig = Sig $r
    if ($null -eq $cur -or $cur.Sig -ne $sig) {
        if ($cur) { $segments += $cur }
        $cur = [pscustomobject]@{ Sig = $sig; Rows = New-Object System.Collections.ArrayList; First = $r }
    }
    [void]$cur.Rows.Add($r)
}
if ($cur) { $segments += $cur }

# ---- header -----------------------------------------------------------------------------------
Write-Host ""
Write-Host "==== Reprojection telemetry ====" -ForegroundColor Cyan
Write-Host ("file        : {0}" -f $Path)
Write-Host ("stat windows: {0}   events: {1}   segments: {2}" -f $stats.Count, $events.Count, $segments.Count)
if ($stats.Count -gt 0) {
    $dur = ([double]$stats[-1].t_ms - [double]$stats[0].t_ms) / 1000.0
    Write-Host ("duration    : {0:N1} s" -f $dur)
}

# ---- per-segment ------------------------------------------------------------------------------
$idx = 0
foreach ($seg in $segments) {
    $idx++
    $r0 = $seg.First
    $ia = @($seg.Rows | ForEach-Object { [double]$_.input_age_ms })
    $ga = @($seg.Rows | ForEach-Object { [double]$_.game_age_ms })
    $ji = @($seg.Rows | ForEach-Object { [double]$_.jitter_ms })
    $pf = @($seg.Rows | ForEach-Object { [double]$_.present_fps })
    $gf = @($seg.Rows | ForEach-Object { [double]$_.game_fps })
    $gd = @($seg.Rows | ForEach-Object { [double]$_.gpu_depth })
    $rf = [double]$r0.refresh_hz
    $secs = $seg.Rows.Count    # ~1 window/sec
    $missDelta = [double]$seg.Rows[-1].missed_vblanks - [double]$seg.Rows[0].missed_vblanks
    $missPerSec = if ($secs -gt 1) { $missDelta / ($secs - 1) } else { $missDelta }

    # trust gate: presents phase-locked to the refresh?
    $pfMed = Med $pf
    $lockRatio = if ($rf -gt 1) { $pfMed / $rf } else { 0 }
    $jitCap = if ($rf -gt 1) { 1000.0 / $rf * 0.5 } else { 5.0 }
    $ji99 = Pct $ji 99
    # Break the phase-lock test into its specific failure reasons so the verdict is actionable.
    $reasons = @()
    if ($lockRatio -lt 0.95) { $reasons += ("under-refresh ({0:N0}/{1:N0}fps={2:P0})" -f $pfMed, $rf, $lockRatio) }
    if ($lockRatio -gt 1.05) { $reasons += "over-refresh (vsync off?)" }
    if ($missPerSec -ge 2.0) { $reasons += ("{0:N0} missed/s" -f $missPerSec) }
    if ($ji99 -ge $jitCap)   { $reasons += ("jitter p99 {0:N1}>{1:N1}ms" -f $ji99, $jitCap) }
    $phaseLocked = ($reasons.Count -eq 0)
    $verdict = if ($phaseLocked) { "TRUSTWORTHY" } else { "SUSPECT: " + ($reasons -join ", ") }
    $vColor  = if ($phaseLocked) { "Green" } else { "Yellow" }

    $en  = if ($r0.enabled -eq '1') { "on" } else { "OFF" }
    $lw  = if ($r0.latewarp -eq '1') { "on" } else { "off" }
    $al  = if ($r0.autolead -eq '1') { "auto" } else { "manual" }
    $vs  = if ($r0.vsync -eq '1') { "on" } else { "off" }
    $gainStr = if ($r0.angular -eq '1') { "angular sens={0} fov={1}" -f $r0.sens, $r0.fov } else { "uvgain={0}" -f $r0.gain }

    Write-Host ""
    Write-Host ("--- segment {0}  ({1:N0}s) ---" -f $idx, $secs) -ForegroundColor Cyan
    Write-Host ("  warp={0} mode={1} {2} sign={3} | latewarp={4} lead={5}({6}) floor={7} maxfif={8} vsync={9}" -f `
        $en, $r0.mode, $gainStr, $r0.sign, $lw, $al, ([math]::Round((Med (@($seg.Rows | ForEach-Object{[double]$_.lead_ms}))),2)), $r0.lead_floor_ms, $r0.maxfif, $vs)
    Write-Host ("  present {0:N0} fps   game {1:N0} fps   refresh {2:N0} Hz" -f $pfMed, (Med $gf), $rf)
    Write-Host ("  input->scanout : med {0,5:N2}  p99 {1,5:N2}  min {2,5:N2} ms" -f (Med $ia), (Pct $ia 99), (Pct $ia 1))
    Write-Host ("  game-frame age : med {0,5:N2}  p99 {1,5:N2} ms" -f (Med $ga), (Pct $ga 99))
    Write-Host ("  jitter         : med {0,5:N2}  p99 {1,5:N2} ms   missed {2:N1}/s   gpuDepth {3:N1}" -f (Med $ji), (Pct $ji 99), $missPerSec, (Med $gd))
    Write-Host ("  latency verdict: {0}" -f $verdict) -ForegroundColor $vColor
}

# ---- event timeline ---------------------------------------------------------------------------
Write-Host ""
Write-Host "==== events ====" -ForegroundColor Cyan
foreach ($e in $events) {
    Write-Host ("  {0,8:N1}s  {1}" -f ([double]$e.t_ms/1000.0), $e.event)
}
Write-Host ""
Write-Host "note: 'input->scanout' is latch->present-return; it is only a real photon latency when the" -ForegroundColor DarkGray
Write-Host "      segment is TRUSTWORTHY (presents phase-locked). A SUSPECT segment means jitter/missed" -ForegroundColor DarkGray
Write-Host "      vblanks make that number meaningless -- raise the lead floor until it goes green." -ForegroundColor DarkGray
