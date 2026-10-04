"""Fresh-child latency and OS high-water RSS, never cumulative child usage."""
import os
import subprocess
import sys
import tempfile
import threading
import time

def run_measured(command,work,timeout=120):
    env=dict(os.environ)
    for key in ('KAWA_NO_OPT','KAWA_NO_PRINTF','KAWA_DUMP_BAD'):
        env.pop(key,None)
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        started=time.perf_counter_ns()
        process=subprocess.Popen([str(x) for x in command],cwd=work,env=env,stdout=stdout,stderr=stderr)
        timer=threading.Timer(timeout,process.kill)
        timer.daemon=True
        timer.start()
        _,status,usage=os.wait4(process.pid,0)
        elapsed=(time.perf_counter_ns()-started)/1e6
        timer.cancel()
        process.returncode=os.waitstatus_to_exitcode(status)
        stdout.seek(0); stderr.seek(0)
        output,error=stdout.read(),stderr.read()
        if process.returncode:
            raise RuntimeError(f'{command}: exit {process.returncode}\n{output.decode(errors="replace")}{error.decode(errors="replace")}')
        if error:
            raise RuntimeError(f'{command}: unexpected stderr\n{error.decode(errors="replace")}')
        return output,{'elapsed_ms':elapsed,
            'peak_process_rss_bytes':int(usage.ru_maxrss)*(1 if sys.platform=='darwin' else 1024),
            'user_cpu_ms':usage.ru_utime*1000,'system_cpu_ms':usage.ru_stime*1000}
