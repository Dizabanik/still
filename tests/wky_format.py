"""lit adapter; execution is shared with scripts/test.py."""
from pathlib import Path
import sys
import lit.Test
import lit.formats

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from test_support import run_case


class WkyTestFormat(lit.formats.FileBasedTest):
    def __init__(self, still_path):
        self.still = still_path

    def execute(self, test, litConfig):
        results = run_case(test.getSourcePath(), self.still)
        statuses = {r["status"] for r in results}
        status = next(s for s in ("FAIL", "XPASS", "XFAIL", "PASS", "UNSUPPORTED") if s in statuses)
        detail = "\n".join(f"O{r.get('optimization', '-')}: {r['status']}\n{r['detail']}" for r in results)
        return getattr(lit.Test, status), detail
