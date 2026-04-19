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
