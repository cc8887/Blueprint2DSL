# BlueprintLisp

BlueprintLisp 是一个 Editor-only Unreal Engine 插件，用 Lisp 风格的 S-expression DSL 表达 Blueprint 图。它可以把二进制 Blueprint 图导出为适合 AI 阅读和修改的文本，也可以将修改后的 DSL 校验并写回资产。

## 当前支持的功能

### Blueprint 图与 DSL 双向转换

- 支持命名图的导出与导入：`EventGraph`、`FunctionGraph`、`MacroGraph`。
- 支持直接返回 DSL 文本、读写指定 `.bplisp` 文件，以及导出到默认目录：
  `Saved/BP2DSL/BlueprintLisp/<资产相对路径>/<图名>.bplisp`。
- 导出时可选择保留节点位置 `:pos`，并默认生成稳定的 `:event-id` / `:id`。
- 导入后可选择编译并保存 Blueprint；Python 桥接接口默认执行自动布局生命周期请求。

当前覆盖的常用图结构包括：

- 普通事件、自定义事件、父类或接口事件、Input Action、Input Key、组件绑定事件，以及 `LevelScriptBlueprint` 中的 Actor 绑定事件。
- 函数、宏、父类调用、宏调用、普通函数调用、返回节点和局部变量。
- 顺序执行、分支、动态转换，以及 `int`、`string`、`enum` switch。
- 成员变量读写、结构体创建/拆分/字段修改、数组创建/取值、`select`、纯函数表达式等。
- 顶层 `var` 声明的创建，以及常用默认值、Instance Editable、Expose on Spawn 元数据。

实际 Blueprint 节点类型很多，不能假定任意节点都能无损往返。请先在目标资产副本上导出、导入并重新导出，确认结构和编译结果符合预期。

### 语法检查与查询

Python 桥接类 `unreal.BlueprintLispPythonBridge` 提供：

- `validate_dsl`：解析语法并检查允许的顶层 form，不修改资产；嵌套调用、引用和 Pin 兼容性仍需在导入及 Blueprint 编译阶段验证。
- `list_graphs`：列出 Blueprint 中可访问的图。
- `inspect_member_variable` / `list_member_variables`：查询成员变量及其可重建能力。
- `export_stub`：导出 UK2Node pin 签名存根，用于 lint、提示和 AI 上下文。

插件也公开 C++ AST、Parser、Pretty Printer 和 Converter API，便于在其他 Editor 插件中集成。

### 局部图修改与文本 Diff

导出时保留稳定 ID 后，可以使用 `MergeAppend` 导入模式更新目标事件、函数或宏。导入器会收集匹配入口覆盖的下游执行子图及其纯依赖，再按 `:event-id` / `:id`、节点类型和语义兼容性复用已有节点并重连内容；不兼容的节点会新建，该入口覆盖范围内未被新 DSL 消费的旧节点会删除。回归用例覆盖了事件入口、函数调用、纯表达式、数组、分支、Cast、Macro、Switch 等常用节点的 ID 复用。

因此，修改通常只表现为 `.bplisp` 中相关表达式的文本差异，更适合 Git diff、代码审查和 AI 定点修改。使用时应遵守两个条件：

- 从 `bStableIds=true` 的导出结果开始编辑，并保留未改节点的 ID。
- 通过 `import_graph_from_text(..., MERGE_APPEND, ...)` 写回；先在副本上验证编译和回导出结果。

`MergeAppend` 是针对目标入口覆盖子图的稳定 ID 导入复用，不是保留整张图所有旧节点，也不是完整的语义 diff 引擎。当前 `UpdateGraphFromText`、`UpdateGraphFromFile`、C++ `Update()` 和 `UpdateSemantic` 模式尚未实现，请不要调用它们。

### 导入模式

- `ReplaceGraph`：清理目标图中可替换的节点后重建，适合完整覆盖；这是默认模式。
- `MergeAppend`：保留未涉及的其他入口，并重写目标入口覆盖的子图；兼容节点可按稳定 ID 复用，适合局部修改。
- `UpdateSemantic`：枚举值已预留，但当前未实现。

## 支持的引擎版本

| 范围 | 状态 |
| --- | --- |
| UE 4.27 | 代码兼容目标，需在目标工程中编译验证 |
| UE 5.1 - 5.8 | 代码兼容目标 |
| UE 5.5 | 本仓库明确验证版本 |
| UE 5.0 | 不在支持范围内 |

除 UE 5.5 外，其余版本仍应结合目标项目使用的节点、插件和平台自行验证。`BlueprintLisp.uplugin` 声明支持 Win64、Mac 和 Linux；本插件不包含运行时模块，所有能力仅在 Editor 中使用。

## 安装

仓库提供插件源码，不提供跨引擎版本通用的预编译二进制。目标机器需要安装对应 Unreal Engine 版本的 C++ 编译工具链。

### 安装到项目

1. 关闭 Unreal Editor。
2. 将仓库下载或克隆到项目插件目录，并确保目录结构如下：

   ```text
   <Project>/
     Plugins/
       BlueprintLisp/
         BlueprintLisp.uplugin
         Source/
   ```

   使用 Git 时可直接执行：

   ```powershell
   git clone https://github.com/cc8887/Blueprint2DSL.git "<Project>\Plugins\BlueprintLisp"
   ```

3. 如果需要通过 Python 或 AI 调用，在 Unreal Editor 的 Plugins 面板中同时启用内置的 **Python Editor Script Plugin**。
4. 重新生成项目文件并编译项目的 Editor Target；也可以打开项目并在 Unreal 提示时选择重新编译插件。
5. 启动 Editor，在 Plugins 面板确认 **BlueprintLisp - Blueprint DSL Engine** 已启用，然后重启 Editor。

也可以把插件放到 `<UE>/Engine/Plugins/Marketplace/BlueprintLisp/` 供多个项目使用，但每个引擎版本都需要分别编译。切换引擎版本时，建议先删除插件目录中的 `Binaries/` 和 `Intermediate/`，再用目标引擎重新生成并编译。

## 最小使用流程

以下代码可在 Unreal Python Console 或 Editor Python 脚本中运行：

```python
import unreal

bridge = unreal.BlueprintLispPythonBridge
bp = "/Game/Path/BP_Example.BP_Example"
graph = "EventGraph"

# 导出时保留稳定 ID，便于后续做局部修改。
exported = bridge.export_graph_to_text(bp, graph, False, True)
if not exported.success:
    raise RuntimeError(exported.message)

dsl_text = exported.dsl_text
# 在这里修改 dsl_text。

checked = bridge.validate_dsl(dsl_text)
if not checked.success:
    raise RuntimeError(checked.message)

result = bridge.import_graph_from_text(
    bp,
    graph,
    dsl_text,
    unreal.BlueprintLispPythonImportMode.MERGE_APPEND,
    True,   # compile
    True,   # save package
)
if not result.success:
    raise RuntimeError(result.message)
```

首次导入一个完整图，或不需要保留现有节点时，应使用：

```python
unreal.BlueprintLispPythonImportMode.REPLACE_GRAPH
```

## 可选集成

BlueprintLisp 会在导入各阶段广播生命周期事件。其他 Editor 插件可以注册 hook；例如自动排版插件可在 `PostNodeChanges` 阶段处理导入器请求的 `AutoLayout` 行为。没有安装对应集成插件时，BlueprintLisp 的导入仍可正常执行，只是不提供该插件实现的附加行为。

## 配套 AI Skill

BlueprintLisp 的配套 AI Skill 收录在公开仓库 [UE-Editor-MCPServer-Skills](https://github.com/cc8887/UE-Editor-MCPServer-Skills) 中：

- Git：`https://github.com/cc8887/UE-Editor-MCPServer-Skills.git`
- Skill 直达：[plugins/blueprint-lisp](https://github.com/cc8887/UE-Editor-MCPServer-Skills/tree/main/plugins/blueprint-lisp)

该 Skill 是通过 MCP 和 `unreal.BlueprintLispPythonBridge` 调用本插件的可选自动化层，不是 BlueprintLisp 的编译或运行依赖。插件当前支持范围和能力边界以本 README 与当前代码为准。

## 测试

自动化测试位于 `Source/BlueprintLisp/Private/Tests/`，资产级回归脚本位于 `Tests/Regression/`。运行回归脚本前，可设置默认 Editor 和工程路径：

```powershell
$env:BLUEPRINTLISP_EDITOR_CMD = "<UnrealEditor-Cmd.exe 的绝对路径>"
$env:BLUEPRINTLISP_UPROJECT = "<验证工程 .uproject 的绝对路径>"
$env:BLUEPRINTLISP_CASE_FILTER = "<适用于当前工程的 case id，可用逗号分隔>"
python .\Tests\Regression\run_blueprintlisp_regression_suite.py
```

部分 manifest case 自带 `uproject` 配置并依赖特定样例资产；case 级配置优先于上述默认环境变量。运行前应把这些 case 的 `uproject` 改为自己的验证工程或移除该字段，或者通过 `BLUEPRINTLISP_CASE_FILTER` 仅选择已经适配当前工程的用例。
