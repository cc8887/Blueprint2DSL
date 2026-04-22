# BlueprintLisp Regression Tests

这组文件是从本地验证脚本整理出的可分享测试分支版本，做过两类处理：

1. 去掉了本机用户名与本地绝对路径；输出文件默认落在脚本所在目录。
2. 把回归运行入口改成相对路径 + 环境变量驱动，避免把本地 Unreal / 项目路径写进仓库。

## 目录说明

- `blueprintlisp_regression_manifest.json`：回归清单（相对脚本路径）
- `run_blueprintlisp_regression_suite.py`：批量运行入口
- `*.py`：正式回归用例脚本
- `villager_select_before_print.bplisp`：`repair_villager_select_symbol_test.py` 依赖的输入夹具
- `workbench/`：Edge Move 相关的探索/诊断脚本

## 运行方式

先设置环境变量，再执行：

- `BLUEPRINTLISP_EDITOR_CMD`：`UnrealEditor-Cmd.exe` 的绝对路径
- `BLUEPRINTLISP_UPROJECT`：验证用 `.uproject` 的绝对路径

示例：

```powershell
$env:BLUEPRINTLISP_EDITOR_CMD = "<你的 UnrealEditor-Cmd.exe 绝对路径>"
$env:BLUEPRINTLISP_UPROJECT = "<你的验证工程 .uproject 绝对路径>"
python .\Tests\Regression\run_blueprintlisp_regression_suite.py
```

如果需要，也可以直接在 `blueprintlisp_regression_manifest.json` 中填入 `editor_cmd` / `uproject`。

另外，manifest 里的单个 case 也可以单独覆盖 `uproject` / `editor_cmd`。当前增量 EventGraph probe 就是这样做的：默认回归仍可跑 Cropout，而少数 AnimBP / ALS 定向用例会自动切到 `D:\MCP\MCP.uproject`。

如果只想跑某一组 case，可以临时设置环境变量 `BLUEPRINTLISP_CASE_FILTER`，值为逗号分隔的 case id 列表。例如只跑新增的增量 EventGraph probe：

```powershell
$env:BLUEPRINTLISP_CASE_FILTER = "incremental_root_event_reuse,incremental_call_reuse,incremental_pure_call_reuse,incremental_call_macro_reuse,incremental_set_branch_reuse,incremental_cast_reuse,incremental_switch_int_reuse,incremental_call_parent_reuse,incremental_input_action_reuse,incremental_input_key_reuse,incremental_component_bound_event_reuse,incremental_actor_bound_event_reuse,incremental_function_seq_reuse,incremental_macro_exit_generic_reuse"
python .\Tests\Regression\run_blueprintlisp_regression_suite.py





```


