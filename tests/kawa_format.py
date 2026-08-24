import os
import re
import shutil
import subprocess
import tempfile

import lit.Test
import lit.formats


class KawaTestFormat(lit.formats.FileBasedTest):
    """Golden-output and IR-shape testing with no in-file boilerplate."""

    def __init__(self, kawac_path):
        self.kawac = kawac_path

    def execute(self, test, litConfig):
        test.config.environment["KAWA_NO_OPT"] = ""
        src = test.getSourcePath()

        # Dispatch on layout.
        if os.path.sep + "ir" + os.path.sep in src:
            return self._run_ir(test)
        return self._run_golden(test)

    # -- golden output ----------------------------------------------------

    def _run_golden(self, test):
        import subprocess

        src = test.getSourcePath()
        expected_path = src + ".out"
        if not os.path.exists(expected_path):
            return (lit.Test.UNSUPPORTED, "no %s twin\n" % expected_path)

        work = tempfile.mkdtemp(prefix="kawa-golden-")
        name = os.path.splitext(os.path.basename(src))[0]
        exe = os.path.join(work, name)

        try:
            comp = subprocess.run(
                [self.kawac, src, "-o", exe],
                capture_output=True, text=True, timeout=60, cwd=work)
            if comp.returncode != 0:
                return (
                    lit.Test.FAIL, "kawac failed:\n%s%s" % (comp.stdout, comp.stderr))

            run = subprocess.run(
                [exe], capture_output=True, text=True, timeout=30)
            actual = run.stdout
            with open(expected_path) as f:
                expected = f.read()
            if run.returncode != 0:
                return (
                    lit.Test.FAIL, "binary exited %d\nstdout:\n%s\nstderr:\n%s"
                    % (run.returncode, actual, run.stderr))
            if actual != expected:
                import difflib
                diff = "".join(difflib.unified_diff(
                    expected.splitlines(True), actual.splitlines(True),
                    fromfile="expected", tofile="actual"))
                return (lit.Test.FAIL, diff)
            return (lit.Test.PASS, '')
        except subprocess.TimeoutExpired:
            return (lit.Test.FAIL, "timeout")
        finally:
            import shutil
            shutil.rmtree(work, ignore_errors=True)

    # -- IR shape via FileCheck -------------------------------------------

    def _run_ir(self, test):
        import subprocess

        src = test.getSourcePath()
        with open(src) as f:
            text = f.read()

        # CHECK directives live in comment lines; strip the comment marker
        # so FileCheck sees them. Supports `//` comments.
        checks = []
        opt_level = "0"
        for line in text.splitlines():
            m = re.match(r"\s*//\s*(CHECK.*|OPT:.*)$", line)
            if not m:
                continue
            body = m.group(1)
            om = re.match(r"OPT:\s*([0-3])\s*$", body)
            if om:
                opt_level = om.group(1)
                continue
            checks.append(body)
        if not any(c.startswith("CHECK") for c in checks):
            return (
                lit.Test.UNSUPPORTED, "no CHECK directives\n")

        work = tempfile.mkdtemp(prefix="kawa-ir-")
        ll = os.path.join(work, "output.ll")
        check_file = os.path.join(work, "checks.txt")
        with open(check_file, "w") as f:
            f.write("\n".join(checks) + "\n")

        try:
            comp = subprocess.run(
                [self.kawac, "-O" + opt_level, "-c", src],
                capture_output=True, text=True, timeout=60,
                cwd=work)
            if comp.returncode != 0:
                return (
                    lit.Test.FAIL, "kawac failed:\n%s%s" % (comp.stdout, comp.stderr))
            # kawac writes output.ll into its cwd.
            produced = os.path.join(work, "output.ll")
            if not os.path.exists(produced):
                return (lit.Test.FAIL, "no output.ll produced\n")
            fc = subprocess.run(
                ["FileCheck", "--input-file", produced, check_file],
                capture_output=True, text=True)
            if fc.returncode != 0:
                return (
                    lit.Test.FAIL, fc.stdout + fc.stderr +
                    "\n--- generated IR ---\n" +
                    open(produced).read())
            return (lit.Test.PASS, '')
        except subprocess.TimeoutExpired:
            return (lit.Test.FAIL, "timeout")
        finally:
            import shutil
            shutil.rmtree(work, ignore_errors=True)

