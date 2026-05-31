# BlueprintLisp

BlueprintLisp 是一个 Unreal Editor 插件，用 S-expression DSL 在 Blueprint 图与文本之间做双向转换，便于 AI 读写、审查、生成和批量修改蓝图逻辑。

## 支持范围

- 图类型：EventGraph、FunctionGraph、MacroGraph
- 模块类型：Editor-only
- 平台：Win64 / Mac / Linux
- 当前已验证引擎版本：UE 5.5
- 其他引擎版本：未在本仓库声明，使用前请自行验证

## 安装

1. 把插件放到 `<你的项目>/Plugins/BlueprintLisp/`
2. 重新生成项目文件并编译 Editor
3. 打开 Unreal Editor，启用插件
4. 如果要让 AI 通过 Python 调用，请同时启用 Python Editor Script Plugin

## AI 如何使用

AI 不直接改二进制蓝图，而是通过 `unreal.BlueprintLispPythonBridge` 读写 DSL。

推荐流程：

1. `list_graphs`：先列出蓝图里有哪些图
2. `export_graph_to_text`：把目标图导出成 DSL
3. AI 修改 DSL 文本
4. `validate_dsl`：先做语法校验
5. `import_graph_from_text` 或 `update_graph_from_text`：把结果写回蓝图

最小示例：

```python
import unreal

bp = "/Game/Path/BP_Example.BP_Example"
graph = "EventGraph"

result = unreal.BlueprintLispPythonBridge.export_graph_to_text(bp, graph, False, True)
dsl_text = result.dsl_text

check = unreal.BlueprintLispPythonBridge.validate_dsl(dsl_text)
if not check.success:
    raise RuntimeError(check.message)

# AI 修改 dsl_text 后再写回
unreal.BlueprintLispPythonBridge.update_graph_from_text(bp, graph, dsl_text, True, True)
```

## 常用接口

- `list_graphs(BlueprintPath)`：列出图名
- `export_graph_to_text(BlueprintPath, GraphName, bIncludePositions, bStableIds)`：导出到文本
- `export_graph_to_file(...)`：导出到指定文件
- `export_graph_to_default_path(...)`：导出到默认目录 `Saved/BP2DSL/BlueprintLisp/...`
- `import_graph_from_text(...)`：从 DSL 覆盖/追加导入
- `update_graph_from_text(...)`：按稳定 ID 做增量更新
- `validate_dsl(DSLText)`：只校验，不改资产
- `export_stub()`：导出节点存根，便于做 lint / 提示补全

## 基本使用

### 1. 导出图到文本

```python
import unreal
res = unreal.BlueprintLispPythonBridge.export_graph_to_default_path(
    "/Game/Path/BP_Example.BP_Example",
    "EventGraph",
    False,
    True,
)
print(res.file_path)
```

### 2. 从文本写回图

```python
import unreal

bp = "/Game/Path/BP_Example.BP_Example"
graph = "EventGraph"
dsl_text = "(event-graph)"

res = unreal.BlueprintLispPythonBridge.import_graph_from_text(
    bp,
    graph,
    dsl_text,
    unreal.EBlueprintLispPythonImportMode.REPLACE_GRAPH,
    True,
    True,
)
print(res.message)
```

## 测试

可移植回归脚本位于：

- `Tests/Regression/`

批量运行时，先设置：

- `BLUEPRINTLISP_EDITOR_CMD`
- `BLUEPRINTLISP_UPROJECT`

再执行：

```powershell
python .\Tests\Regression\run_blueprintlisp_regression_suite.py
```

## 相关插件

- **AnimBP2FP**：动画蓝图 ⇄ AnimLang DSL 转换
- **MatBP2FP**：材质蓝图 ⇄ MatLang DSL 转换
- **BlueprintAutoLayout**：图节点自动排版。BlueprintLisp 通过导入生命周期钩子（`PostNodeChanges` 阶段）与之集成——当导入上下文请求 `AutoLayout` 行为时，导入完成后会自动整理新增/变更节点的布局，无需手动排版。该集成是可选的：BlueprintAutoLayout 未启用时，导入流程正常工作，只是不自动排版。

## 注意

- 建议先 `validate_dsl`，再导入
- 建议保留稳定 ID，用于增量更新
- 首次接入时，先在测试蓝图验证，再批量改正式资产
