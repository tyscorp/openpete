#!/usr/bin/env bash

# tests/harness/check_record_minimize.sh - a recording survives a minimized window.
#
# Minimizing the window gives the surface a zero extent. A recording must keep
# rendering and encoding through it at the size it started at, and after the
# restore. The window opens at the exe's default size, so it stays on one
# monitor. The Windows exe
# is the only build where a scripted minimize reaches the swapchain (WSLg and
# xvfb ignore the request), so this runs the windows-mingw exe through WSL
# interop and minimizes it with PowerShell ShowWindow.
#
#   leg rec    --record-cfr 60: once the log prints `record: armed` the window
#              is minimized for 1 s, then restored. The offline capture clock
#              runs unpaced, so this leg runs to a later --frame-limit than
#              norec: at 1700 it exits under 1 s after arming, before a
#              minimize sent then lands. PASS needs exit 0, a `record:
#              finalized` line with frames captured and 0 dropped, no `native
#              size changed`, and a clip whose decoded frame count equals the
#              captured count.
#   leg norec  the same run without recording, to --frame-limit 1700: exit 0.
#
# Engagement, per leg: the minimize counts once the swapchain's zero-extent
# deferral line appears, and the restore once a swapchain is created after
# that line. Each is sent up to 3 times, waiting 2 s (minimize) or 3 s
# (restore) for its line; a leg whose minimize or restore is not confirmed
# before the run exits is VACUOUS, with the reason in <leg>.min. One
# PowerShell helper per leg is started before the launch, so the ShowWindow
# call does not wait on PowerShell start-up.
#
# Usage: tests/harness/check_record_minimize.sh [--exe PATH] [--out DIR]
# Exit 0 PASS, 1 FAIL, 2 VACUOUS, 3 REFUSED (an openpete process already runs on
# Windows), 64 SKIP (no exe, no powershell.exe or no disc).

# xvfb-guard: windows-only (the run is the Windows .exe on the Windows desktop)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
EXE="$ROOT/build/windows-mingw/openpete.exe"
OUT="$ROOT/build/check-record-minimize"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --exe) EXE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "check_record_minimize: unknown argument $1" >&2; exit 64 ;;
    esac
done
DISC="$ROOT/games/spyro-2/spyro-2/build/SCUS-94425.cue"
[[ -x "$EXE" ]] || { echo "check_record_minimize: SKIP: no exe at $EXE"; exit 64; }
[[ -f "$DISC" ]] || { echo "check_record_minimize: SKIP: no disc at $DISC"; exit 64; }
command -v powershell.exe >/dev/null || { echo "check_record_minimize: SKIP: no powershell.exe (not WSL)"; exit 64; }

. "$ROOT/scripts/lib/sandbox.sh"
. "$ROOT/scripts/lib/wslenv.sh"
wslenv_forward OPENPETE_VIBRATION=0
mkdir -p "$OUT"
rm -f "${OUT:?}"/rec.log "${OUT:?}"/norec.log "${OUT:?}"/clip-rec.mkv "${OUT:?}"/minwin.ps1 \
    "${OUT:?}"/rec.min "${OUT:?}"/norec.min

# minwin.ps1 reads one command per line: `min` (ShowWindow SW_MINIMIZE) or
# `restore` (SW_RESTORE) on the window of the newest openpete process,
# answering `<cmd> iconic=<0|1>` once IsIconic matches or after 0.5 s, or
# `no-window` when no openpete window appears within 5 s; `quit` ends it.
cat > "$OUT/minwin.ps1" <<'EOF'
Add-Type -Name W -Namespace U -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h,int c); [DllImport("user32.dll")] public static extern bool IsIconic(System.IntPtr h);'
function Say($s) { [Console]::Out.WriteLine($s); [Console]::Out.Flush() }
function Find-Window {
    for ($i = 0; $i -lt 50; $i++) {
        $p = Get-Process openpete -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Sort-Object StartTime -Descending | Select-Object -First 1
        if ($p) { return $p.MainWindowHandle }
        Start-Sleep -Milliseconds 100
    }
    return [IntPtr]::Zero
}
Say "ready"
$h = [IntPtr]::Zero
while ($null -ne ($cmd = [Console]::In.ReadLine())) {
    if ($cmd -eq "quit") { break }
    if ($h -eq [IntPtr]::Zero) { $h = Find-Window }
    if ($h -eq [IntPtr]::Zero) { Say "no-window"; continue }
    $min = ($cmd -eq "min")
    [U.W]::ShowWindow($h, $(if ($min) { 6 } else { 9 })) | Out-Null
    for ($i = 0; $i -lt 20 -and [U.W]::IsIconic($h) -ne $min; $i++) { Start-Sleep -Milliseconds 25 }
    Say "$cmd iconic=$([int][U.W]::IsIconic($h))"
}
EOF

# wait_for SECONDS PID CMD...: polls CMD every 50 ms. Returns 0 once it
# succeeds, 2 when PID exits first (CMD is checked once more after the exit),
# 1 at the timeout.
wait_for() {
    local secs=$1 pid=$2 i
    shift 2
    for ((i = 0; i < secs * 20; i++)); do
        "$@" && return 0
        kill -0 "$pid" 2>/dev/null || { "$@" && return 0; return 2; }
        sleep 0.05
    done
    return 1
}
has_zero() { grep -q "surface extent 0x0" "$1"; }
# has_rebuild LOG: a swapchain was created after the zero-extent deferral line.
has_rebuild() { awk '/surface extent 0x0/ { z = 1 } z && /^\[rhi\/vk\] swapchain: [0-9]+ image/ { f = 1 } END { exit !f }' "$1"; }

# win_cmd CMD: sends CMD to the helper and sets reply to its answer.
win_cmd() {
    reply=""
    echo "$1" >&"${WIN[1]}" 2>/dev/null || { reply="helper gone"; return; }
    read -r -t 10 reply <&"${WIN[0]}" || reply="helper timeout"
    reply=${reply%$'\r'}
}

# engage LEG LOG PID NAME CMD SECS CHECK: sends CMD until CHECK LOG holds,
# up to 3 times, waiting SECS after each; appends `NAME: confirmed ...` or
# `NAME: unconfirmed: <reason>` to LEG.min and returns 0 on confirmation.
engage() {
    local leg=$1 log=$2 pid=$3 name=$4 cmd=$5 secs=$6 check=$7 n rc=1
    for ((n = 1; n <= 3; n++)); do
        win_cmd "$cmd"
        wait_for "$secs" "$pid" "$check" "$log"; rc=$?
        (( rc == 1 )) || break
    done
    case $rc in
        0) echo "$name: confirmed (attempt $n, helper: $reply)" >> "$OUT/$leg.min"; return 0 ;;
        2) echo "$name: unconfirmed: the run exited first (attempt $n, helper: $reply)" >> "$OUT/$leg.min" ;;
        *) echo "$name: unconfirmed: no log line after 3 attempts (helper: $reply)" >> "$OUT/$leg.min" ;;
    esac
    return 1
}

# run_leg LEG: starts the minimize helper, launches the exe, waits for the
# arming line (rec) or for the level skip to resume presents (norec; during
# the skip's fast-forward no frame begins, so a minimize there never reaches
# the swapchain), minimizes until the deferral line appears, holds 1 s,
# restores until a swapchain is rebuilt, and waits for the exit.
run_leg() {
    local leg=$1 log="$OUT/$1.log" extra=() limit=1700 trigger="level playable - presents resume" pid wpid ready=""
    if [[ $leg == rec ]]; then
        extra=(--record-cfr 60 --record "$(wslpath -w "$OUT")\\clip-rec.mkv")
        limit=3000 trigger="record: armed"
    fi
    : > "$log"; : > "$OUT/$leg.min"
    coproc WIN { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$OUT/minwin.ps1")" 2>&1; }
    wpid=$WIN_PID   # bash unsets WIN_PID when the helper exits
    read -r -t 30 ready <&"${WIN[0]}"
    env "${WSLENV_ARGS[@]}" "$EXE" --game spyro-2 --no-config --disc "$(wslpath -w "$DISC")" \
        --renderer vk --no-card --mute 1 --skip-to-level glimmer \
        --frame-limit "$limit" --input-lock --vibration 0 --background 1 "${extra[@]}" > "$log" 2>&1 &
    pid=$!
    if [[ ${ready%$'\r'} != ready ]]; then
        echo "minimize: unconfirmed: the PowerShell helper did not start (${ready:-no output})" >> "$OUT/$leg.min"
    elif ! wait_for 60 "$pid" grep -q "$trigger" "$log"; then
        echo "minimize: unconfirmed: no '$trigger' line before the exit or 60 s" >> "$OUT/$leg.min"
    elif engage "$leg" "$log" "$pid" minimize min 2 has_zero; then
        sleep 1
        engage "$leg" "$log" "$pid" restore restore 3 has_rebuild
    fi
    echo quit >&"${WIN[1]}" 2>/dev/null
    wait "$pid"
    echo "exit=$?" >> "$OUT/$leg.min"
    wait "$wpid" 2>/dev/null
}

# Refuse while another openpete process runs on Windows: its window would be
# a minimize target and the legs would race a session in use (exit 3).
running=$(powershell.exe -NoProfile -Command "(Get-Process openpete -ErrorAction SilentlyContinue | Measure-Object).Count" 2>/dev/null | tr -d '\r')
if [[ ${running:-0} -gt 0 ]]; then
    echo "check_record_minimize: REFUSED: $running openpete process(es) already running on Windows"
    exit 3
fi

verdict=0
run_leg rec
run_leg norec
for leg in rec norec; do
    log="$OUT/$leg.log"
    grep -q "^exit=0" "$OUT/$leg.min" || { echo "FAIL $leg: $(grep exit= "$OUT/$leg.min")"; verdict=1; }
    for step in minimize restore; do
        grep -q "^$step: confirmed" "$OUT/$leg.min" && continue
        echo "VACUOUS $leg: $(grep -m1 "^$step: " "$OUT/$leg.min" || echo "$step: not attempted (the minimize was not confirmed)")"
        (( verdict == 0 )) && verdict=2
    done
done
fin=$(grep -oE "[0-9]+ frame\(s\) captured, [0-9]+ dropped" "$OUT/rec.log" | tail -1)
cap=${fin%% *}; drop=$(sed -E 's/.*captured, ([0-9]+) dropped/\1/' <<< "$fin")
if [[ -z "$fin" || "$drop" != 0 || "${cap:-0}" -eq 0 ]]; then echo "FAIL rec: finalized '${fin:-none}'"; verdict=1; fi
grep -q "native size changed" "$OUT/rec.log" && { echo "FAIL rec: the recorder saw a size change"; verdict=1; }
if [[ -f "$OUT/clip-rec.mkv" ]]; then
    dec=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT/clip-rec.mkv")
    [[ "$dec" == "$cap" ]] || { echo "FAIL rec: clip decodes $dec frames, the recorder captured $cap"; verdict=1; }
else
    echo "FAIL rec: no clip"; verdict=1
fi
case $verdict in
    0) echo "check_record_minimize: PASS (captured $cap, dropped 0; both legs exit 0 through a confirmed minimize and restore)" ;;
    1) echo "check_record_minimize: FAIL" ;;
    2) echo "check_record_minimize: VACUOUS" ;;
esac
exit $verdict
