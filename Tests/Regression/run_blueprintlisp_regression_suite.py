import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
MANIFEST_PATH = os.path.join(ROOT, "blueprintlisp_regression_manifest.json")
RESULT_PATH = os.path.join(ROOT, "blueprintlisp_regression_suite_result.json")


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def save_json(path, payload):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)


def resolve_path(path_value):
    if not path_value:
        return path_value
    if os.path.isabs(path_value):
        return path_value
    return os.path.join(ROOT, path_value)


def read_case_result(path):
    if not os.path.exists(path):
        return None, f"结果文件不存在: {path}"
    try:
        return load_json(path), ""
    except Exception as exc:
        return None, f"读取结果文件失败: {exc}"


def build_status(expected_success, actual_success):
    if actual_success is None:
        return "fail", False
    if expected_success and actual_success:
        return "pass", True
    if (not expected_success) and (not actual_success):
        return "xfail", True
    if (not expected_success) and actual_success:
        return "unexpected_pass", False
    return "fail", False


def main():
    manifest = load_json(MANIFEST_PATH)
    default_timeout = int(manifest.get("default_timeout_seconds", 900))
    default_editor_cmd = manifest.get("editor_cmd") or os.environ.get("BLUEPRINTLISP_EDITOR_CMD") or os.environ.get("UE_EDITOR_CMD")
    default_uproject = manifest.get("uproject") or os.environ.get("BLUEPRINTLISP_UPROJECT") or os.environ.get("UE_UPROJECT_PATH")
    if not default_editor_cmd:
        raise RuntimeError("缺少 editor_cmd，请在 manifest 中填写或设置环境变量 BLUEPRINTLISP_EDITOR_CMD")

    common_args = manifest.get("common_args", [])

    if not isinstance(common_args, list):
        raise RuntimeError("manifest.common_args 必须是数组")
    cases = manifest.get("cases", [])
    case_filter_text = os.environ.get("BLUEPRINTLISP_CASE_FILTER", "").strip()
    if case_filter_text:
        requested_case_ids = [item.strip() for item in case_filter_text.split(",") if item.strip()]
        requested_case_set = set(requested_case_ids)
        cases = [case for case in cases if case.get("id") in requested_case_set]
        if not cases:
            raise RuntimeError(f"BLUEPRINTLISP_CASE_FILTER 未命中任何 case: {case_filter_text}")
    else:
        requested_case_ids = []

    suite_report = {

        "success": False,
        "suite_name": manifest.get("suite_name", "BlueprintLisp Regression Cases"),
        "manifest_path": MANIFEST_PATH,
        "case_filter": requested_case_ids,
        "started_at_epoch": int(time.time()),

        "cases": [],
        "errors": [],
    }

    overall_success = True

    for case in cases:
        case_id = case["id"]
        script_path = resolve_path(case["script_path"])
        result_path = resolve_path(case["result_path"])
        expected_success = bool(case.get("expected_success", True))
        timeout_seconds = int(case.get("timeout_seconds", default_timeout))
        log_path = resolve_path(case.get("log_path", f"{case_id}_cmd.log"))
        extra_args = case.get("extra_args", [])
        if not isinstance(extra_args, list):
            raise RuntimeError(f"case.extra_args 必须是数组: {case_id}")

        case_editor_cmd = case.get("editor_cmd") or default_editor_cmd
        case_uproject = case.get("uproject") or default_uproject
        if not case_uproject:
            raise RuntimeError(f"缺少 uproject，请在 manifest 顶层、case 内或环境变量 BLUEPRINTLISP_UPROJECT 中填写: {case_id}")

        command = [
            case_editor_cmd,
            case_uproject,
            *common_args,
            *extra_args,
            "-run=pythonscript",
            f"-script={script_path}",
            "-unattended",
            "-nop4",
            "-nosplash",
            "-NoSound",
            "-stdout",
            "-FullStdOutLogOutput",
        ]

        case_report = {

            "id": case_id,
            "target": case.get("target", ""),
            "issue": case.get("issue", ""),
            "notes": case.get("notes", ""),
            "script_path": script_path,
            "result_path": result_path,
            "log_path": log_path,
            "expected_success": expected_success,
            "timeout_seconds": timeout_seconds,
            "editor_cmd": case_editor_cmd,
            "uproject": case_uproject,
            "command": command,
        }


        try:
            completed = subprocess.run(
                command,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout_seconds,
                check=False,
            )
            combined_output = (completed.stdout or "") + ("\n" if completed.stdout and completed.stderr else "") + (completed.stderr or "")
            with open(log_path, "w", encoding="utf-8") as f:
                f.write(combined_output)
            case_report["command_exit_code"] = completed.returncode
            case_report["command_timed_out"] = False
        except subprocess.TimeoutExpired as exc:
            combined_output = (exc.stdout or "") + ("\n" if exc.stdout and exc.stderr else "") + (exc.stderr or "")
            with open(log_path, "w", encoding="utf-8") as f:
                f.write(combined_output)
            case_report["command_exit_code"] = None
            case_report["command_timed_out"] = True
            case_report["result_read_error"] = f"命令超时: {timeout_seconds}s"
            case_report["actual_success"] = None
            case_report["status"], case_report["matched_expectation"] = build_status(expected_success, None)
            suite_report["cases"].append(case_report)
            overall_success = False
            continue

        result_payload, result_error = read_case_result(result_path)
        if result_error:
            case_report["result_read_error"] = result_error
            actual_success = None
        else:
            case_report["case_result"] = result_payload
            actual_success = bool(result_payload.get("success"))

        case_report["actual_success"] = actual_success
        case_report["status"], case_report["matched_expectation"] = build_status(expected_success, actual_success)
        overall_success = overall_success and case_report["matched_expectation"]
        suite_report["cases"].append(case_report)

    suite_report["finished_at_epoch"] = int(time.time())
    suite_report["success"] = overall_success
    save_json(RESULT_PATH, suite_report)

    print(json.dumps(suite_report, ensure_ascii=False, indent=2))
    return 0 if overall_success else 1


if __name__ == "__main__":
    sys.exit(main())
