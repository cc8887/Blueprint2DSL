# BlueprintLisp Regression Tests

## 资产无关的语义保真回归（UE 5.8）

`Source/BlueprintLisp/Private/Tests/BlueprintLispSemanticTests.cpp` 从原生 K2 节点构造最小源图，再导出、导入到全新 Blueprint、编译并执行。覆盖外部对象及结构体拆分读取、class 参数和容器、同名调用的多输出归属、纯节点多输出共享以及静默损失拒绝。测试不依赖 WUCHANG 语料或生产资产。

先在宿主工程编译插件，再运行：

```powershell
python .\Tests\Regression\run_semantic_regression.py `
  --editor "<UnrealEditor-Cmd.exe>" --project "<Host.uproject>" `
  --output "<一个新的结果目录>"
```

`--filter BlueprintLisp` 可运行整个 BlueprintLisp Automation 测试集。`--save-fixtures` 会把测试资产保存到宿主的 `/Game/BlueprintLispTests/Round1/<本次唯一编号>/`，路径记录在结果目录的 `fixtures.json`；随后设置 `BLUEPRINTLISP_FIXTURE_MANIFEST` 为该文件绝对路径，通过新 Editor 进程运行 `verify_semantic_reload.py`，验证磁盘重载后的类型和行为。可用 `BLUEPRINTLISP_RELOAD_RESULT` 指定重载报告路径。输出目录保留 Automation 报告、Editor 日志与 JSON 摘要。宿主插件若在退出阶段阻塞，运行器只会在完整测试报告落盘后终止自己启动的进程，并在摘要中单独记录。

这些测试证明对应夹具的编译和运行行为，不证明完整游戏逻辑、所有 K2 节点或所有引擎版本的兼容性。

这组文件是从本地验证脚本整理出的可分享测试分支版本，做过两类处理：

1. 去掉了本机用户名与本地绝对路径；输出文件默认落在脚本所在目录。
2. 把回归运行入口改成相对路径 + 环境变量驱动，避免把本地 Unreal / 项目路径写进仓库。

## 目录说明

- `blueprintlisp_regression_manifest.json`：回归清单（相对脚本路径）
- `run_blueprintlisp_regression_suite.py`：批量运行入口
- `*.py`：正式回归用例脚本
- `villager_select_before_print.bplisp`：`repair_villager_select_symbol_test.py` 依赖的输入夹具
- `workbench/`：Edge Move 相关的探索/诊断脚本

`timeline_semantics_validate` 是资产无关的语法回归：它只调用 `validate_dsl`，不读取、创建或保存 Blueprint 资产。仍需提供一个能够加载 BlueprintLisp 插件的 `.uproject` 来启动 Editor 命令行。

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
$env:BLUEPRINTLISP_CASE_FILTER = "incremental_root_event_reuse,incremental_call_reuse,incremental_pure_call_reuse,incremental_pure_array_reuse,incremental_break_struct_reuse,incremental_generic_pure_enum_reuse,incremental_call_macro_reuse,incremental_set_branch_reuse,incremental_cast_reuse,incremental_switch_int_reuse,incremental_call_parent_reuse,incremental_input_action_reuse,incremental_input_key_reuse,incremental_component_bound_event_reuse,incremental_actor_bound_event_reuse,incremental_function_seq_reuse,incremental_macro_exit_generic_reuse"

python .\Tests\Regression\run_blueprintlisp_regression_suite.py
```

单独运行 Timeline 语法回归：

```powershell
$env:BLUEPRINTLISP_CASE_FILTER = "timeline_semantics_validate"
python .\Tests\Regression\run_blueprintlisp_regression_suite.py
```


