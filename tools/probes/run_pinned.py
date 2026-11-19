"""Run a command pinned to the P cores (affinity 0xFFF on a 13600K) with a timeout; append stdout to a log.

   python run_pinned.py <log> <timeout s> <exe> [args...]

With GPUCTR=1 in the environment it also samples the child's "GPU Engine" utilisation counters once per second.
"""
import ctypes
import os
import subprocess
import sys
import threading

log, timeout, cmd = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
k32 = ctypes.windll.kernel32
h = k32.OpenProcess(0x0200 | 0x0400, False, p.pid)  # PROCESS_SET_INFORMATION | QUERY_INFORMATION
ok = k32.SetProcessAffinityMask(h, 0xFFF)
k32.CloseHandle(h)

gpu = []


def sample_gpu():
    ps = ("$ErrorActionPreference='SilentlyContinue'; while ($true) { $s = Get-Counter -Counter "
          r"'\GPU Engine(*)\Utilization Percentage'"
          " -MaxSamples 1; if (-not $s) { break }; "
          "$s.CounterSamples | Where-Object { $_.InstanceName -like 'pid_" + str(p.pid) + "_*' } | ForEach-Object { '{0} {1:N1}' -f $_.InstanceName, $_.CookedValue } ; '---' }")
    q = subprocess.Popen(['powershell', '-NoProfile', '-Command', ps], stdout=subprocess.PIPE, text=True)
    try:
        for line in q.stdout:
            if p.poll() is not None:
                break
            gpu.append(line.strip())
    finally:
        q.kill()


if os.environ.get('GPUCTR'):
    threading.Thread(target=sample_gpu, daemon=True).start()
try:
    out, _ = p.communicate(timeout=timeout)
except subprocess.TimeoutExpired:
    p.kill()
    out, _ = p.communicate()
    out += b'\n[TIMEOUT]\n'
text = out.decode('utf-8', 'replace')
if gpu:
    agg = {}
    for line in gpu:
        if ' ' in line and line != '---':
            inst, val = line.rsplit(' ', 1)
            try:
                agg.setdefault(inst.split('engtype_')[-1], []).append(float(val))
            except ValueError:
                pass
    text += '\nGPU engine utilisation of this process (1 s samples, per engine instance):\n'
    for eng, vals in sorted(agg.items()):
        nz = [v for v in vals if v > 0]
        text += '  %-12s samples %d  max %.1f%%  mean of non-zero %.1f%%\n' % (
            eng, len(vals), max(vals), sum(nz) / len(nz) if nz else 0.0)
with open(log, 'a', encoding='utf-8') as f:
    f.write('$ ' + ' '.join(cmd) + '   (affinity 0xFFF set=%s, rc=%s)\n' % (bool(ok), p.returncode) + text + '\n')
sys.stdout.write(text)
sys.exit(p.returncode or 0)
