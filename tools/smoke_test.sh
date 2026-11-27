#!/usr/bin/env bash
# Smoke test: launch a CRYENGINE 5.7.1 source build's GameLauncher (or Sandbox) on a minimal
# project that loads the CEVPlayer plugin, run some console commands, quit, report PASS/FAIL.
#
#   tools/smoke_test.sh <engine-root> [options]
#
#   --sandbox            launch Sandbox.exe instead of GameLauncher.exe
#   --seconds N          idle N seconds after the last command, then `quit`      (default 10)
#   --delay N            wait N seconds after startup before the first command   (default 2)
#   --exec "c1;c2;..."   console commands to run, in order. Separate with ';'.
#                        Include `wait_seconds N` entries yourself to space them out,
#                        e.g. --exec "cev_play Videos/vp8_vorbis.webm;wait_seconds 5;cev_stop"
#   --project FILE       .cryproject (default $CEV_PROJECT, else D:/coding11/_testproject/CEVTest/CEVTest.cryproject)
#   --timeout N          hard timeout in seconds; the process is killed and the run FAILs
#                        (default 300; the very first run on a tree is slower (~1 min: rc.exe converts
#                        EngineAssets .tif textures, shaders compile into <project>/user/shaders))
#   --out DIR            where logs go (default <project dir>/../smoke_logs/<stamp>_<engine>_<mode>)
#   --expect REGEX       extra: log must match REGEX (repeatable)
#   --forbid REGEX       extra: log must NOT match REGEX (repeatable)
#   --no-plugin-check    do not require the "CEVPlayer" plugin-loaded lines in the log
#   --no-logfile         do not pass -logfile (the engine then writes <engine-root>/game.log)
#   --keep-cfg           keep the generated Assets/_smoke_*.cfg
#   --tail N             lines of log tail to print (default 40)
#
# Exit status: 0 = PASS, 1 = FAIL (crash / hang / non-zero exit / missing expectation), 2 = usage/setup.
#
# How the commands get run: the launcher is started with `+then exec _smoke_<pid>.cfg`. `then`
# defers `exec`, and an exec'd file under deferred mode is queued line by line, so the file
#     wait_seconds <delay> / <cmds...> / wait_seconds <seconds> / quit
# is processed from CXConsole::Update() once the engine is running (plain `+cmd` args run too early,
# per source reading:
# at CryAction init, before plugins register their commands). NB: stock GameLauncher returns exit
# code 1 after a clean quit (CryInitializeEngine returns false after the main loop), so PASS is judged
# from the log ("quit" executed + "System Shutdown"), plus: no hard timeout, no crash exit code
# (anything other than 0/1), no error.log / *.dmp, no crash pattern in the log.
# --sandbox: the editor never runs the console deferred queue, so commands go via an immediate `+exec`
# and smoke_dialog_watch.ps1 closes the editor window (WM_CLOSE) after delay+seconds. A blocking dialog
# (CryMessageBox, Assertion Failed, crash handler) is detected by the same watcher, dumped to dialog.txt, killed.
set -uo pipefail

usage() { sed -n 2,/^set\ -uo/p "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }

[ $# -ge 1 ] || usage
case "$1" in -h|--help) usage ;; esac
engine="$1"; shift

mode=launcher; seconds=10; delay=2; cmds=""; timeout_s=300; out=""; tailn=40
project="${CEV_PROJECT:-D:/coding11/_testproject/CEVTest/CEVTest.cryproject}"
plugin_check=1; keep_cfg=0; use_logfile=1
expects=(); forbids=()
while [ $# -gt 0 ]; do
  case "$1" in
    --sandbox) mode=sandbox ;;
    --seconds) seconds="$2"; shift ;;
    --delay) delay="$2"; shift ;;
    --exec) cmds="$2"; shift ;;
    --project) project="$2"; shift ;;
    --timeout) timeout_s="$2"; shift ;;
    --out) out="$2"; shift ;;
    --expect) expects+=("$2"); shift ;;
    --forbid) forbids+=("$2"); shift ;;
    --no-plugin-check) plugin_check=0 ;;
    --keep-cfg) keep_cfg=1 ;;
    --no-logfile) use_logfile=0 ;;
    --tail) tailn="$2"; shift ;;
    -h|--help) usage ;;
    *) echo "smoke_test: unknown option $1" >&2; usage ;;
  esac
  shift
done

[ -d "$engine" ] || { echo "smoke_test: engine root $engine not found" >&2; exit 2; }
engine="$(cd "$engine" && pwd)"
bin="$engine/bin/win_x64"
case "$mode" in
  launcher) exe="$bin/GameLauncher.exe" ;;
  sandbox)  exe="$bin/Sandbox.exe" ;;
esac
[ -f "$exe" ] || { echo "smoke_test: $exe not built" >&2; exit 2; }
[ -f "$project" ] || { echo "smoke_test: project $project not found" >&2; exit 2; }
project="$(cd "$(dirname "$project")" && pwd)/$(basename "$project")"
pdir="$(dirname "$project")"
[ -f "$bin/CEVPlayer.dll" ] || echo "smoke_test: WARNING $bin/CEVPlayer.dll missing - plugin cannot load" >&2

stamp="$(date +%Y%m%d_%H%M%S)"
ename="$(basename "$engine")"
[ -n "$out" ] || out="$pdir/../smoke_logs/${stamp}_${ename}_${mode}"
mkdir -p "$out"; out="$(cd "$out" && pwd)"
log="$out/Game.log"
marker="$out/.start_marker"; : > "$marker"

# ---- generate the exec script (lives in the project's Assets dir so CryPak finds it)
assets="$pdir/Assets"; [ -d "$assets" ] || assets="$pdir"
cfgname="_smoke_$$.cfg"
cfg="$assets/$cfgname"
{
  [ "$mode" = sandbox ] || echo "wait_seconds $delay"
  if [ -n "$cmds" ]; then
    IFS=';' read -ra arr <<< "$cmds"
    for c in "${arr[@]}"; do
      c="$(echo "$c" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
      [ -n "$c" ] && echo "$c"
    done
  fi
  [ "$mode" = sandbox ] || { echo "wait_seconds $seconds"; echo "quit"; }
} > "$cfg"
cleanup() { [ "$keep_cfg" = 1 ] || rm -f "$cfg"; }
trap cleanup EXIT

echo "== smoke_test: $mode  engine=$engine"
echo "   project=$project"
echo "   commands: ${cmds:-<none>}  (delay ${delay}s, idle ${seconds}s, hard timeout ${timeout_s}s)"
echo "   out=$out"
sed 's/^/   cfg| /' "$cfg"

wproject="$(cygpath -w "$project")"
# -logfile is joined onto the engine-root cwd (PathUtil::Make(cwd, name)), so it must be a path RELATIVE to the engine root
wlog="$(realpath --relative-to="$engine" "$log")"
if [ "$mode" = sandbox ]; then
  # Sandbox never runs the console deferred queue (CSystem::Update skips console update with ESYSUPDATE_EDITOR),
  # so `then exec` is dead there: use an immediate `+exec` (commands run at editor init, before most plugin
  # commands exist) and let the watcher close the window instead of a scripted quit.
  args=(-project "$wproject" +sys_no_crash_dialog 1 +exec "$cfgname"); closeafter=$((seconds + delay))
else
  args=(-project "$wproject" +sys_no_crash_dialog 1 +then exec "$cfgname"); closeafter=0
fi
[ "$use_logfile" = 1 ] && args+=(-logfile "$wlog")
# without -logfile the launcher writes game.log into the engine root (CryInitializeEngine chdirs there);
# remove stale ones first so the log we read belongs to this run. A second concurrent instance
# would write game(1).log, so do not run two smoke tests on one engine tree at once.
[ "$use_logfile" = 1 ] || rm -f "$engine"/game.log "$engine"/game\(*\).log 2>/dev/null

# watcher: a blocking error dialog (e.g. "CRYENGINE FATAL ERROR") would otherwise hang until the hard timeout
procname="$(basename "$exe" .exe)"
dlg="$out/dialog.txt"; rm -f "$dlg" "$dlg.started"
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$(dirname "$0")/smoke_dialog_watch.ps1")" \
  -ProcName "$procname" -OutFile "$(cygpath -w "$dlg")" -LogFile "$(cygpath -w "$([ "$use_logfile" = 1 ] && echo "$log" || echo "$engine/game.log")")" -CloseAfter "$closeafter" -MaxSeconds "$timeout_s" > /dev/null 2>&1 &
watcher=$!
t0=$(date +%s)
# Run in the background and poll: a crashed GameLauncher can end up as an un-killable zombie
# (observed: process stuck after "Saved dump file", even taskkill /F fails), and a plain `timeout`
# would then block forever. We give up waiting at timeout+20s and report it.
rcfile="$out/.rc"; rm -f "$rcfile"
( cd "$bin" && timeout -k 5 "$timeout_s" "$exe" "${args[@]}" > "$out/stdout.txt" 2>&1; echo $? > "$rcfile" ) &
runner=$!
waited=0
while [ ! -s "$rcfile" ] && [ "$waited" -lt $((timeout_s + 20)) ]; do
  # watcher saw a blocking dialog / crash handler: give the process 8 s to die, then stop waiting
  if [ -s "$dlg" ] && [ "$waited" -ge "${dlg_seen:-99999}" ]; then break; fi
  [ -s "$dlg" ] && [ -z "${dlg_seen:-}" ] && dlg_seen=$((waited + 8))
  sleep 1; waited=$((waited + 1))
done
if [ -s "$rcfile" ]; then rc=$(cat "$rcfile"); else
  rc=999; taskkill //F //IM "$(basename "$exe")" > /dev/null 2>&1; kill "$runner" 2>/dev/null
fi
kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null; rm -f "$dlg.started"
t1=$(date +%s)
dur=$((t1 - t0))

# ---- collect crash artifacts: anything named error.log / *.dmp / *.mdmp created since start
artifacts=()
while IFS= read -r f; do artifacts+=("$f"); done < <(
  find "$out" "$engine" "$bin" "$pdir" -maxdepth 3 -type f \
       \( -iname 'error.log' -o -iname '*.dmp' -o -iname '*.mdmp' -o -iname 'crash*.xml' \) \
       -newer "$marker" -not -path '*/Code/*' 2>/dev/null | sort -u)

# without --no-logfile, take the engine-root game.log
if [ "$use_logfile" != 1 ]; then
  src="$engine/game.log"
  [ -f "$src" ] || src="$(ls -t "$engine"/game*.log 2>/dev/null | head -1)"
  [ -n "$src" ] && [ -f "$src" ] && cp "$src" "$log"
fi

# NOTE: stock GameLauncher.exe returns EXIT_FAILURE (1) after a *clean* quit, because
# CryInitializeEngine() returns false once RunMainLoop() has finished. So exit code 0 and 1 are both
# "ran to the end"; the real graceful-quit evidence is the log (quit command executed + System Shutdown).
reasons=()
if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
  reasons+=("hard timeout (${timeout_s}s) hit, process killed: hang, or the engine never reached the quit command")
elif [ "$rc" -eq 999 ]; then
  reasons+=("process did not exit by itself (hung in crash handler / blocking dialog, or no exit within timeout+20s); it was force-killed")
elif [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ]; then
  reasons+=("abnormal exit code $rc (0x$(printf '%X' "$rc" 2>/dev/null)) - crash/abort")
fi
[ -s "$log" ] || reasons+=("no game log produced at $log")
if [ -s "$dlg" ]; then
  reasons+=("blocking dialog appeared (process killed by watcher): $(tr '\n' '|' < "$dlg" | cut -c1-300)")
fi
crash_re='Exception Code|Unhandled [Ee]xception|ACCESS_VIOLATION|has crashed|CrashRpt|Stack overflow|FATAL ERROR|Assertion failed'
if [ -s "$log" ] && grep -E -q "$crash_re" "$log"; then
  reasons+=("crash pattern in log: $(grep -E -m1 "$crash_re" "$log" | cut -c1-120)")
fi
if [ ${#artifacts[@]} -gt 0 ]; then
  for a in "${artifacts[@]}"; do
    cp -f "$a" "$out/$(basename "$a")" 2>/dev/null || true
    reasons+=("crash artifact: $a")
  done
fi
if [ -s "$log" ]; then
  if [ "$mode" != sandbox ]; then
    grep -q "Executing console command 'quit'" "$log" || reasons+=("log shows the scripted 'quit' never ran (engine died early, or stalled before the console queue)")
  fi
  grep -q -E 'System Shutdown|System:Quit' "$log" || reasons+=("log has no 'System Shutdown' / 'System:Quit' line - no graceful shutdown")
fi
if [ "$plugin_check" = 1 ] && [ -s "$log" ]; then
  grep -q -i "CEVPlayer" "$log" || reasons+=("log has no 'CEVPlayer' line - plugin not loaded?")
fi
for e in "${expects[@]+"${expects[@]}"}"; do
  grep -E -q "$e" "$log" 2>/dev/null || reasons+=("expected log pattern not found: $e")
done
for f in "${forbids[@]+"${forbids[@]}"}"; do
  grep -E -q "$f" "$log" 2>/dev/null && reasons+=("forbidden log pattern present: $f")
done

echo "-- exit code: $rc   wall time: ${dur}s"
echo "-- CEVPlayer / plugin lines:"
grep -E "Loading plug-in CEVPlayer|Unloading CEVPlayer|\[CEVPlayer\]|flownode 'Video|Executing console command 'cev_" "$log" 2>/dev/null \
  | grep -v -E "<CVideoPlayer> (---|Header|Pos|Size|Track|TimeCode|Position)" | head -40 | sed "s/^/   | /"
nerr=$(grep -c "\[Error\]" "$log" 2>/dev/null); nwarn=$(grep -c "\[Warning\]" "$log" 2>/dev/null)
echo "-- log: ${nerr:-0} [Error] ($(grep -c -F '[CVARS]' "$log" 2>/dev/null) of them stock [CVARS] group noise), ${nwarn:-0} [Warning]; other errors:"
grep -F '[Error]' "$log" 2>/dev/null | grep -v -F '[CVARS]' | head -8 | sed "s/^/   E| /"
echo "-- last $tailn log lines (shader-leak noise filtered) of $log:"
grep -v "leak after level unload" "$log" 2>/dev/null | tail -n "$tailn" | sed "s/^/   | /"; echo
[ -s "$out/stdout.txt" ] && { echo "-- stdout/stderr:"; head -n 10 "$out/stdout.txt" | sed 's/^/   | /'; }

if [ ${#reasons[@]} -eq 0 ]; then
  echo "RESULT: PASS ($mode, ${dur}s, clean quit, no crash artifacts)"
  exit 0
fi
echo "RESULT: FAIL ($mode, ${dur}s)"
for r in "${reasons[@]}"; do echo "   - $r"; done
exit 1
