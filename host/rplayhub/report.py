"""Write run reports into the repo.

Everything that touches the tunnel has to run under sudo, so its output would otherwise be
stranded in whatever terminal it was typed in. A report on disk means a run can be inspected
after the fact — by a person, by the next session, or by a tool.
"""
import json
import pathlib

REPORT_DIR = pathlib.Path(__file__).resolve().parents[2] / "logs"


def write_report(name: str, payload: dict) -> pathlib.Path:
    REPORT_DIR.mkdir(exist_ok=True)
    path = REPORT_DIR / name
    path.write_text(json.dumps(payload, indent=2, sort_keys=True, default=str))
    try:
        # Written as root; leave it readable and removable by the owner.
        path.chmod(0o644)
    except OSError:
        pass
    return path
