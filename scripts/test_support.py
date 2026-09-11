"""Shared, dependency-free test execution for lit and the standalone runner."""
import difflib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile


def environment(extra=None):
    env = dict(os.environ)
    for key in ("KAWA_NO_OPT", "KAWA_NO_PRINTF", "KAWA_DUMP_BAD"):
        env.pop(key, None)
    env.update(LC_ALL="C", LANG="C", NO_COLOR="1")
    env.update(extra or {})
    return env


def invoke(command, cwd, timeout=60, stdin=b"", env=None):
    """Kill the entire process group on timeout, including compiler children."""
    with subprocess.Popen(command, cwd=cwd, env=env or environment(),
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, start_new_session=True) as p:
        try:
            out, err = p.communicate(stdin, timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.communicate()
            raise
        return subprocess.CompletedProcess(command, p.returncode, out, err)


def load_case(src):
    src = Path(src)
    sidecar = Path(str(src) + ".json")
    if sidecar.exists():
        spec = json.loads(sidecar.read_text())
    elif "ir" in src.parts:
        body = src.read_text()
        opt = re.search(r"^\s*//\s*OPT:\s*([0-3])\s*$", body, re.M)
        flags = re.search(r"^\s*//\s*FLAGS:\s*(.*)$", body, re.M)
        import shlex
        spec = {"kind": "ir", "optimizations": [int(opt.group(1)) if opt else 0],
                "flags": shlex.split(flags.group(1)) if flags else []}
    elif Path(str(src) + ".out").exists():
        spec = {"kind": "run"}
    else:
        return None
    if not isinstance(spec, dict) or spec.get("kind") not in {"run", "reject", "trap", "ir"}:
        raise ValueError("unknown test kind")
    spec.setdefault("optimizations", [0, 2, 3])
    spec.setdefault("flags", ["--bounds-check=safe"])
    if not spec["optimizations"] or any(o not in range(4) for o in spec["optimizations"]):
        raise ValueError("optimizations must be a nonempty list of levels 0..3")
    if spec.get("xfail") and not spec["xfail"].get("reason"):
        raise ValueError("expected failures require a reason")
    if spec.get("xfail"):
        failure = spec["xfail"]
        if failure.get("phase") not in {"output", "exit", "diagnostic", "ir", "acceptance", "trap", "stderr"}:
            raise ValueError("crashes, timeouts, and infrastructure/build failures cannot be expected failures")
        if failure["phase"] != "acceptance" and not failure.get("pattern"):
            raise ValueError("expected failures require a specific failure pattern")
    if "inputs" in spec and (not isinstance(spec["inputs"], list) or not spec["inputs"]):
        raise ValueError("inputs must be a nonempty list")
    if spec.get("inputs") and spec.get("xfail"):
        raise ValueError("split known failures into separate cases instead of masking a multi-input test")
    return spec


def _text(data):
    return data.decode("utf-8", errors="replace")


def _check_variant(src, kawac, spec, opt, work):
    exe = work / "program"
    cmd = [kawac, f"-O{opt}", *spec["flags"], str(src)]
    cmd += ["-c"] if spec["kind"] in {"reject", "ir"} else ["-o", str(exe)]
    compiled = invoke(cmd, work, spec.get("compile_timeout", 60))
    diagnostic = _text(compiled.stdout + compiled.stderr)
    if compiled.returncode < 0 or re.search(r"FATAL|LLVM ERROR|Assertion .*failed|PLEASE submit a bug", diagnostic):
        return "compiler_crash", diagnostic
    if spec["kind"] == "reject":
        if compiled.returncode == 0:
            return "acceptance", "compiler accepted a program required to be rejected"
        if compiled.returncode != 1 or not re.search(spec["diagnostic"], diagnostic):
            return "diagnostic", diagnostic
        return None, ""
    if compiled.returncode:
        return "compile", diagnostic
    if spec["kind"] == "ir":
        ir_path = work / "output.ll"
        if not ir_path.exists():
            return "ir_missing", "compiler did not emit output.ll"
        checks = re.findall(r"^\s*//\s*(CHECK(?:-[A-Z]+)?:.*)$", src.read_text(), re.M)
        if not checks:
            return "configuration", "IR test has no CHECK directives"
        check_file = work / "checks.txt"
        check_file.write_text("\n".join(checks) + "\n")
        result = invoke([os.environ.get("FILECHECK", "FileCheck"),
                         "--input-file", str(ir_path), str(check_file)], work)
        if result.returncode:
            return "ir", _text(result.stdout + result.stderr) + "\n" + ir_path.read_text()
        return None, ""
    if not exe.exists():
        return "executable_missing", "compiler did not produce an executable"
    for inputs in spec.get("inputs", [{}]):
        case = dict(spec, **inputs)
        phase, detail = _check_execution(src, exe, case, work)
        if phase:
            return phase, f"args={case.get('args', [])!r}\n" + detail
    return None, ""


def _check_execution(src, exe, spec, work):
    run = invoke([str(exe), *spec.get("args", [])], work,
                 spec.get("run_timeout", 30), spec.get("stdin", "").encode())
    if spec["kind"] == "trap":
        # A segfault/bus error is never evidence of a checked safety violation.
        allowed = spec.get("exit_codes", [-signal.SIGABRT])
        if (run.returncode in {-signal.SIGSEGV, -signal.SIGBUS} or run.returncode not in allowed or
                not re.search(spec["diagnostic"], _text(run.stderr))):
            return "trap", f"exit={run.returncode}\nstdout={_text(run.stdout)!r}\nstderr={_text(run.stderr)!r}"
        return None, ""
    if run.returncode != spec.get("exit", 0):
        return "exit", f"exit={run.returncode}\n{_text(run.stdout + run.stderr)}"
    expected = spec.get("stdout")
    expected = expected.encode() if expected is not None else Path(str(src) + ".out").read_bytes()
    if run.stdout != expected:
        diff = "".join(difflib.unified_diff(_text(expected).splitlines(True),
                       _text(run.stdout).splitlines(True), "expected", "actual"))
        return "output", f"actual bytes: {run.stdout!r}\n{diff}"
    expected_err = spec.get("stderr", "").encode()
    if run.stderr != expected_err:
        return "stderr", f"expected={expected_err!r}, actual={run.stderr!r}"
    return None, ""


def classify(phase, detail, xfail, opt):
    applicable = xfail and opt in xfail.get("optimizations", [0, 2, 3])
    if phase is None:
        return ("XPASS", xfail["reason"]) if applicable else ("PASS", "")
    if (applicable and phase == xfail.get("phase") and
            re.search(xfail.get("pattern", ".*"), detail, re.S)):
        return "XFAIL", xfail["reason"] + "\n" + detail
    return "FAIL", phase + ": " + detail


def run_case(src, kawac):
    src = Path(src).resolve()
    try:
        spec = load_case(src)
        if spec is None:
            return [{"status": "FAIL", "detail": "no output oracle or case manifest"}]
        results = []
        for opt in spec["optimizations"]:
            with tempfile.TemporaryDirectory(prefix="kawa-test-") as folder:
                try:
                    phase, detail = _check_variant(src, kawac, spec, opt, Path(folder))
                except subprocess.TimeoutExpired:
                    phase, detail = "timeout", "subprocess exceeded its time limit"
                except (OSError, KeyError, ValueError, TypeError, re.error) as exc:
                    phase, detail = "infrastructure", str(exc)
            status, detail = classify(phase, detail, spec.get("xfail"), opt)
            results.append({"optimization": opt, "status": status, "detail": detail})
        return results
    except (OSError, ValueError, TypeError, AttributeError, re.error) as exc:
        return [{"status": "FAIL", "detail": "configuration: " + str(exc)}]
