"""Run asset-independent UE Automation converter tests and retain their report.

The Editor is given 20 seconds to exit after writing its completed report. Some
host project plugins block shutdown; only this script's child is then terminated.
The report records that separately from the Automation test results.
"""
import argparse
import json
import pathlib
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--project", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--filter", default="BlueprintLisp.Semantic")
    parser.add_argument("--save-fixtures", action="store_true")
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    output = pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / "automation" / "index.json"
    if report_path.exists():
        raise RuntimeError("Use a new output directory; a previous report already exists")
    command = [args.editor, args.project, "-NullRHI", "-unattended", "-nop4", "-nosplash", "-nosound",
               "-ExecCmds=Automation RunTests " + args.filter,
               "-TestExit=Automation Test Queue Empty", "-ReportExportPath=" + str(report_path.parent),
               "-abslog=" + str(output / "editor.log")]
    if args.save_fixtures:
        command.append("-BlueprintLispSemanticSaveFixtures")
        fixture_run = "run_" + uuid.uuid4().hex[:12]
        command.append("-BlueprintLispSemanticFixtureRun=" + fixture_run)
        fixtures = {name: "/Game/BlueprintLispTests/Round1/" + fixture_run + "/" + name
                    for name in ["BP_BL_External", "BP_BL_Class", "BP_BL_Outputs"]}
        (output / "fixtures.json").write_text(json.dumps(fixtures, indent=2), encoding="utf-8")
    start = time.monotonic()
    completed_at = None
    report = None
    terminated = False
    with (output / "console.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        while process.poll() is None:
            if report_path.exists():
                try:
                    candidate = json.loads(report_path.read_text(encoding="utf-8-sig"))
                    if candidate.get("inProcess", 0) == 0 and candidate.get("notRun", 0) == 0 and candidate.get("tests"):
                        report = candidate
                        if completed_at is None:
                            completed_at = time.monotonic()
                except (OSError, ValueError):
                    pass
            if completed_at and time.monotonic() - completed_at > 20:
                process.terminate()
                terminated = True
                break
            if time.monotonic() - start > args.timeout:
                process.terminate()
                break
            time.sleep(0.5)
        process.wait(timeout=30)
    if report is None and report_path.exists():
        report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    summary = {
        "success": bool((process.returncode == 0 or terminated) and report and report.get("tests") and report.get("failed", 0) == 0
                        and report.get("notRun", 0) == 0 and report.get("inProcess", 0) == 0),
        "command": command, "exit_code": process.returncode,
        "terminated_after_completed_report": terminated,
        "elapsed_seconds": round(time.monotonic() - start, 2),
        "counts": {key: report.get(key) for key in
                   ["succeeded", "succeededWithWarnings", "failed", "notRun", "inProcess"]} if report else {},
        "tests": [{"name": test.get("fullTestPath"), "state": test.get("state"),
                   "errors": [e.get("event", {}).get("message") for e in test.get("entries", [])
                              if e.get("event", {}).get("type") == "Error"]}
                  for test in report.get("tests", [])] if report else [],
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps({**{key: summary[key] for key in ["success", "exit_code", "terminated_after_completed_report", "counts"]},
                      "failed_tests": [t for t in summary["tests"] if t["state"] != "Success"],
                      "summary_path": str(output / "summary.json")}, indent=2, ensure_ascii=False))
    return 0 if summary["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
