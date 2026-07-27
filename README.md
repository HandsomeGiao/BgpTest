# BGP 网络测试框架

当前实现位于 [`BgpTester/`](BgpTester/)：BGP 核心、JSONL CLI 与 SQLite 持久化使用 portable C++20 实现，默认构建不需要 Qt；Qt 6 桌面界面作为显式可选目标保留。

## BgpTester 功能

- 可选 Qt 桌面界面支持可视化添加、编辑、拖动和删除路由器与链路；
- 可选 Qt 桌面界面支持 AS 分组框、RR 标记、方向性 RR Client/MRAI、eBGP Peer/provider-customer 商业关系与链路状态可视化；
- JSON 拓扑加载、保存与旧拓扑方向字段读取；
- portable 核心通过 `RouterPolicyRegistry` 注册节点级策略，内置 Standard BGP、Configurable Export 与 TFP Version；拓扑继续使用 `plugin` 字段保存策略 ID 和私有 JSON 配置；
- 可选 Qt 桌面目标保留旧源码插件的编译期自动发现；
- 路由器级 TFP 路径版本策略通过依赖/触发向量提前排除必然失效的旧路径；
- 事件驱动 BGP OPEN、UPDATE、NOTIFICATION 仿真；
- Adj-RIB-In、Loc-RIB、Adj-RIB-Out、EBGP、IBGP 和基础 Route Reflector；
- LOCAL_PREF、AS_PATH、MED、会话类型、旧路径稳定性与确定性 tie-break 选路；
- 标准与 TFP 路由器按 customer（200）> peer（100）> provider（50）设置 LOCAL_PREF，并对已分类的入站来源和出站邻居执行 valley-free 出口策略；涉及未指定关系的 eBGP 传播保持原有兼容行为；
- 每邻居 MRAI、可配置的 withdrawal MRAI 旁路、FRR 风格 withdrawal 聚合、链路延迟和逐前缀 stale UPDATE generation guard；
- 运行时节点/链路上下线、前缀发布/撤销与收敛状态；
- 最佳路由、全部路径、Peer 状态检查以及画布逐跳路径高亮；
- BMP 风格实时事件表、全列过滤、JSON 详情、JSONL 和 SQLite 历史日志；
- 完整的无 UI `BgpTesterCli`，可用 JSONL/标准输入完成拓扑编辑、仿真扰动、RIB/Peer/路径查询、全量快照与命令审计；
- 默认无 Qt 的 CLI 场景回归测试；启用 GUI 后另有旧 Qt 核心与界面测试。

## 构建

默认构建只需要 CMake 与 C++20 工具链，不需要安装 Qt。CMake 固定获取轻量的
`nlohmann/json`；SQLite 优先使用系统开发包，未找到时自动获取官方
amalgamation。依赖尚未缓存时，首次配置需要访问这些下载地址。

```powershell
cd BgpTester
.\build.ps1
```

如需桌面界面，再安装带 Core、Gui、Widgets、Sql 和 SQLite 驱动的 Qt 6.5 或
更新版本，并通过 `-Gui` 显式启用 GUI：

```powershell
.\build.ps1 -Gui -QtPrefix C:\path\to\Qt\6.x.x\msvc2022_64
```

生成 GUI 可分发目录（调用 Qt 自带 `windeployqt`）：

```powershell
.\build.ps1 -Gui -QtPrefix C:\path\to\Qt\6.x.x\msvc2022_64 -Deploy
```

详细依赖、界面操作、仿真语义和 JSON 格式见 [`BgpTester/README.md`](BgpTester/README.md)。

无 UI 示例：

```powershell
cd BgpTester
build\Release\bin\BgpTesterCli.exe `
  --topology topo\sample_topology.json `
  --script topo\sample_headless_commands.jsonl
```

同一次 `-Gui` 构建仍会生成 CLI；桌面程序位于
`build\Release\bin\BgpTester.exe`。

## 目录

```text
.
├─ BgpTester/       # portable C++20 核心/CLI 与可选 Qt 6 GUI
└─ misc/            # RFC 与实验辅助文件
```
