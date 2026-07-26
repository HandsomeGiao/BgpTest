# BgpTester

`BgpTester` 是统一完成拓扑编辑与 BGP 测试的 Qt 6 / C++20 桌面程序。拓扑编辑、BGP 仿真、运行时扰动、RIB/Peer 检查、路径高亮和 BMP 风格日志浏览都位于同一窗口。

## 依赖

- CMake 3.25 或更新版本
- 支持 C++20 的编译器
- Qt 6.5 或更新版本，需安装 `Core`、`Gui`、`Widgets`、`Network`、`Sql` 模块、SQLite driver，以及仅回归测试使用的 `Test` 模块
- 与 Qt kit 匹配的构建工具（MinGW kit 通常使用 `mingw32-make`，其他 kit 可使用 Ninja）

工程不使用 vcpkg，也不会自动下载依赖。Qt 与编译器由使用者自行安装；请确保所选编译器与 Qt kit 匹配，并把该 kit 的 `bin` 目录加入 `PATH`。`build.ps1` 会优先按 Qt kit 记录的 GCC 主版本，从 Qt 安装目录的 `Tools` 中选择对应 MinGW，避免误用 PATH 中 ABI 不匹配的编译器。

## 构建

在 PowerShell 中：

```powershell
cd BgpTester
.\build.ps1
```

构建并复制 Qt 运行库到输出目录：

```powershell
.\build.ps1 -Configuration Release -Deploy
```

也可以手动配置：

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:\path\to\Qt\6.x.x\mingw_64" `
  -DCMAKE_CXX_COMPILER="C:\path\to\Qt\Tools\mingwXXXX_64\bin\g++.exe"
cmake --build build
ctest --test-dir build --output-on-failure
```

程序位于 `build/bin/BgpTester.exe`。

## 无 UI 命令行模式

构建会同时生成控制台程序 `build/bin/BgpTesterCli.exe`。它使用
`QCoreApplication`，不创建窗口、不读取 GUI 的 `QSettings`，也不依赖显示
服务器。CLI 是一个有状态会话：同一进程内可以依次编辑拓扑、启动仿真、
等待收敛、制造故障、查询 RIB/Peer/路径并导出结果。

最快的完整示例：

```powershell
cd BgpTester
build\bin\BgpTesterCli.exe `
  --topology topo\sample_topology.json `
  --script topo\sample_headless_commands.jsonl
```

当使用 `build.ps1` 的分配置输出目录时，可执行文件位于
`build\Release\bin\BgpTesterCli.exe`。`--script -` 从标准输入读取 JSONL；
不提供 `--script` 或 `--execute` 时会进入命令行交互模式。无参数命令可直接
输入 `help`、`status` 或 `exit`，带参数的命令使用一行一个 JSON 对象：

```jsonl
{"command":"start"}
{"command":"wait_converged","timeout_ms":30000}
{"command":"set_link_state","a":"EDGE","b":"ISP","enabled":false}
{"command":"wait_converged","timeout_ms":30000}
{"command":"get_rib","router":"C1","prefix":"192.0.2.0/24"}
{"command":"stop"}
{"command":"exit"}
```

脚本文件也可以是由命令对象组成的 JSON 数组。`--execute` 可重复，用于简短
的一次性会话；例如 `--execute plugins --execute status`。每次有效命令都先输出
`command_started`，再输出共享同一 `sequence` 的 `command_result`；结果含原命令、
`ok`、`data/error`、时间戳和耗时。这里的命令时间戳与耗时仅是 CLI 墙钟诊断，
不参与仿真调度，也不属于确定性结果。协议解析错误会记录序号、原始输入和来源。
stdout 始终只输出 JSONL，交互提示与诊断写入 stderr。

### 命令能力

| 类别 | 命令 |
|---|---|
| 文件与设置 | `new`、`load`、`save`、`topology`、`validate`、`set_simulation`、`plugins` |
| 路由器编辑 | `add_router`、`update_router`、`move_router`、`delete_router` |
| 链路与批量编辑 | `add_link`、`update_link`、`delete_link`、`batch_update` |
| 仿真控制 | `start`、`stop`、`wait`、`wait_converged`、`status` |
| 运行时扰动 | `set_router_state`、`toggle_router`、`set_link_state`、`toggle_link`、`advertise_prefix`、`withdraw_prefix` |
| 状态检查 | `routers`、`rib`、`peers`、`path`、`snapshot` |
| 日志历史 | `flush_logs`、`query_events`、`query_convergence` |

执行 `--execute help` 可查看每条命令的字段。常用编辑示例：

```jsonl
{"command":"add_router","id":"R3","router_id":"10.0.0.3","asn":65003,"prefixes":["198.51.100.0/24"],"x":700,"y":220}
{"command":"add_link","a":"R2","b":"R3","delay_ms":10,"mrai_ms_from_a":100,"mrai_ms_from_b":100,"relationship":"peer"}
{"command":"update_router","id":"R3","new_id":"EDGE3","asn":65002}
{"command":"set_simulation","withdrawal_ignores_mrai":false}
{"command":"batch_update","router_ids":["R1","EDGE3"],"mrai":{"mode":"random","min_ms":50,"max_ms":100},"delay":{"mode":"fixed","value_ms":10},"seed":12345}
{"command":"save","path":"tmp/edited_topology.json"}
```

路由器重命名会同步修改所有链路端点；删除路由器会级联删除相邻链路；ASN
修改使链路变为 iBGP 时会清除商业关系。`batch_update` 的插件和出站 MRAI
只作用于 `router_ids`（省略时为全部路由器），链路延迟始终作用于全部链路。
随机批量操作会返回并记录种子、种子来源、确定性随机算法版本及每个实际值。未给
`seed` 时，种子由当前规范化拓扑和命令内容经版本化的 `bgptester-canonical-json-v1`
编码稳定派生，不依赖 Qt JSON 输出格式；显式给出相同 `seed` 时，随机整数映射同样不依赖
C++ 标准库实现，因而两种方式都可以跨平台重放。GUI 批量随机
配置也使用输入派生种子，并在状态栏显示算法版本和种子；实际 MRAI/延迟随拓扑保存。

仿真运行期间拓扑编辑会像 GUI 一样锁定。运行时前缀发布、节点/链路状态只
修改本次引擎状态，不会写回待保存的拓扑。交互模式停在提示符时，
仿真与 BMP 落盘仍在独立线程中继续运行。CLI 每条运行时扰动在执行前自动耗尽上一
个协议事件波，再在收敛边界提交；因此 `start` 后立即扰动和显式先执行
`wait_converged` 具有相同语义。`wait_converged.timeout_ms` 为兼容旧脚本继续接受，
但只作为返回的墙钟诊断值，不会在慢机器上截断协议执行。`wait` 的等待时长仍是纯墙钟诊断，
等待结束后返回的协议统计会先进入稳定边界。`status/get_stats`、RIB、Peer、Path、Snapshot 和当前日志查询也会先
进入稳定边界；当前运行期间的任意日志查询（包括通过路径别名访问）以及 `flush_logs` / `exit`
也会先收敛并提交日志；`stop` 会在有界排空后停止，正常收敛时为优雅停止，检测到振荡时记录确定性的中止原因。画布选择、缩放、平移和 Dock
布局是纯显示行为；无 UI 模式以显式目标 ID、`move_router` 和 `path` 分别
替代选择、拖动和路径高亮。

### 数据记录

CLI 保留 GUI 的 BMP 持久化格式。每次 `start` 仍会在
`<log_dir>/<实验名>_<时间>/` 创建：

- `bmp_collector.log`：完整协议/拓扑/收敛事件 JSONL，以 UTF-8 字节和固定 LF 换行写入；
- `bmp_collector.sqlite`：可过滤查询的完整历史；保证逻辑行一致，不承诺不同 SQLite 版本的数据库物理页逐字节一致。

此外 CLI 默认创建 `tmp/cli_sessions/bgptester_cli_<时间>.jsonl`，逐条审计
所有编辑命令、非法命令、幂等操作、查询结果、随机种子和日志绝对路径。
可用 `--record <path>` 指定一个不存在的新文件，或用 `--no-record` 关闭文件审计；
stdout 仍保留同一 JSONL 协议。审计、JSONL 或 SQLite 的任一写入/提交失败
都会传播为命令失败和非零退出码。`snapshot` 会稳定排序并返回所有路由器摘要、
Loc-RIB、本地 RIB、Adj-RIB-In、Peer、逐跳路径、完整路径属性、运行时链路状态
和动态起源前缀；其状态与 `committed_event_id` 在引擎暂停的同一临界区内捕获。
提供 `path` 时还会原子写入独立 JSON 文件。该一致性屏障会在全量复制和日志刷新
期间短暂暂停引擎，因此推荐先执行 `wait_converged` 再导出大型拓扑。

`query_events` 与 GUI 历史过滤使用同一 SQLite 查询实现，并返回完整事件数、
过滤后事件数、BGP 报文数及 `database_max_event_id`；查询当前运行时还返回
`event_run_serial` 和显式刷新的 `committed_event_id`。`query_convergence` 返回每轮触发
事件、持续时间和 BGP 报文数。推荐脚本显式使用 `wait_converged`，不要依赖
固定休眠来判定完成。

脚本默认遇到首个失败命令后停止；`--keep-going` 会继续并最终返回非零状态。
退出码 `0` 表示全部成功，`1` 是 Qt 命令行解析器报告的未知/缺值选项，
`2` 表示命令、协议或运行期记录失败，`3` 表示互斥/重复拓扑参数或审计文件
创建失败，`130` 表示 Ctrl+C。正常退出和 Ctrl+C 都会停止仿真并阻塞刷新日志；
Ctrl+C 也会取消正在构建的大型拓扑运行时。
在某些终端上，交互提示符处的阻塞输入可能需再按一次 Enter 才能让 Ctrl+C
进入收尾流程；自动化请优先通过 JSONL 发送 `exit`。

## 使用

### 编辑拓扑

- `R` 或工具栏“添加路由器”：在画布空白位置单击，填写节点 ID、Router ID、ASN、Cluster ID 与起源前缀。
- `Q` 或“添加链路”：依次单击两台路由器，设置链路延迟、两个方向的 MRAI、RR Client，以及未指定、Peer 或方向性 provider-customer 商业关系。
- `V` 返回选择模式；拖动路由器可调整布局，右键/中键拖动画布，滚轮缩放。
- 双击路由器或链路可编辑；Delete 删除所选对象。
- “编辑 → 批量配置拓扑…”可批量选择 BGP 路由器种类，为目标路由器的全部出站邻居设置固定 MRAI，或为每台路由器独立生成指定闭区间内的随机 MRAI；还可将所有链路延迟设为固定值或逐链路随机值。画布中同时选中至少两台路由器时，路由器种类和 MRAI 只应用到所选节点；否则应用到全部路由器。链路延迟始终应用到全部链路。
- 路由器属性中的“路由器插件”可为每个节点选择不同实现，并以 JSON
  对象保存该插件的私有配置。
- 使用非内置插件的节点会显示蓝色 `P` 徽标，悬停可查看插件 ID。
- 同一 AS 的节点会自动显示在同色分组框中，带 `RR` 徽标的节点至少配置了一个 RR Client。

### 运行仿真

按 `F5` 启动。启动后编辑器会锁定，右侧“运行控制”可执行：

- 节点关闭/恢复；
- 链路断开/恢复；
- 在选定路由器发布或撤销 IPv4 前缀。

“最佳路由”页显示 Loc-RIB；“全部路径”页合并本地路由与 Adj-RIB-In；“邻居”页显示会话状态。选择最佳路由会在画布上高亮逐跳路径。

### 仿真语义

- 事件驱动的 OPEN、UPDATE、NOTIFICATION 处理，不建立真实 TCP 连接；
- Adj-RIB-In、Loc-RIB、Adj-RIB-Out；
- EBGP AS_PATH prepend 与 NEXT_HOP 转换；
- IBGP split-horizon 和基础 Route Reflector client/non-client 传播；
- 标准与 TFP 路由器对已配置商业关系的 eBGP 路由采用 customer（LOCAL_PREF 200）> peer（100）> provider（50）的偏好；当路由来源和出站邻居均已分类时执行 valley-free 出口策略：向 customer 发布全部路由，向 peer/provider 只发布本地起源或从 customer 学到的路由。入站来源或出站邻居为“未指定”时保持原有普通 eBGP 传播行为，便于兼容旧拓扑；
- 为避免商业关系产生的 LOCAL_PREF 跨 AS 泄漏，标准与 TFP 路由器向 eBGP 邻居发送时将其归一为 100，接收端再按自己的已配置邻居关系重新设置；
- 选路顺序：本地起源、LOCAL_PREF、AS_PATH 长度、MED、EBGP 优先、旧路稳定性、确定性 tie-break；
- 每邻居 MRAI；参考 FRR 的 Adj-RIB-Out 待发送队列聚合 withdrawal，同一前缀的新状态覆盖旧状态；
- 同一轮中标准共享路径属性相同的待发布前缀全部聚合到一条 UPDATE，不限制 NLRI 数量；单前缀或元数据完全相同的 TFP Dependency/Trigger 保留为公共属性，只有不同前缀的元数据真正不同时才提升为 sidecar override，因此不会仅因版本信息不同而拆包；
- 全局仿真选项 `withdrawal_ignores_mrai` 控制 withdrawal 是否绕过 MRAI，默认为 `true` 以兼容旧拓扑；关闭后 WITHDRAW 与 UPDATE 共用每邻居 MRAI 计时器。两种模式都会聚合本轮属性相同的待撤销前缀；TFP 公共属性/sidecar 同样保留逐前缀语义，generation guard 过滤前缀时同步过滤对应元数据，不会误丢或串用同批其他路由；
- 若某邻居既没有已提交的 Adj-RIB-Out 路由、待发送项，也没有已 flush 但尚未交付的 generation，撤销路径直接跳过；若存在 in-flight UPDATE，则先取消其 generation，防止后续复活已经撤销的幽灵路由；
- 链路延迟、节点/链路运行时状态以及静默窗口收敛判定；
- 引擎使用单一的整数虚拟时钟，从 `0 ms` 开始。事件以 `(dueAt, insertionOrder)` 构成全序；每次弹出事件时虚拟时钟直接推进到其 `dueAt`，绝不读取系统时钟或等待真实时间；
- 每次事件循环固定处理最多 16384 个事件后让出执行权。批量边界只影响 UI 响应速度，不影响事件顺序、时间或结果；不再使用墙钟预算决定一个批次处理多少事件；
- 每个收敛波最多弹出 10000000 个队列项，无效/过期项也计数。预算在异步和同步路径中共用且不会因重复调用而重置；最后一个预算内事件恰好清空队列时正常收敛，否则记录 `convergence_failed` 并冻结在该确定性边界，拒绝后续链路、节点和前缀变更，直到 `stop`/重新 `start`，避免振荡插件或零延迟循环永久占用线程；
- 所有同步派生事件以当前虚拟时间为因果调度基准，插件 CPU、日志背压和机器负载不会逐跳累加进链路延迟。队列为空时，引擎在同一确定性步骤中推进完整静默窗口并确认收敛；
- 所有来自 `QHash` / `QSet` 的语义遍历都先按稳定的区分大小写 Unicode 顺序规范化；路由指纹使用固定字节编码和固定算法，不依赖 Qt 哈希种子、字长或本机字节序；
- IPv4 地址和 CIDR 只接受四段 ASCII 十进制的规范形式（不接受缩写、前导零、符号或带前导零的前缀长度），不把输入边界交给平台网络解析器；
- 协议事件的 `timestamp` 是固定 UTC 纪元 `2000-01-01T00:00:00Z` 加虚拟毫秒数，`detail.simulation_time_ms` 保存原始整数时间。`converged.duration_ms`（等于 `simulated_active_duration_ms`）止于最后协议活动，`simulated_duration_ms` 包含静默窗口；收敛记录不再混入墙钟字段。

严格一致性的输入边界是：相同拓扑、相同 BgpTester/Qt 构建与内置/第三方插件版本，以及相同顺序的
运行时操作。CLI 自动把每项操作串行化到稳定边界；GUI 在当前事件波收敛前禁用下一项
扰动。直接嵌入 `SimulationEngine` 的调用者应使用 `runUntilConverged()` 建立同样的
边界，或在同一线程中同步提交一个明确有序的批次。日志目录名、CLI 命令耗时等显式
墙钟诊断不属于协议结果；最终 RIB、协议事件序列、虚拟时间、带 `Z` 的 UTC 持久化
时间戳、收敛记录和统计计数属于确定性结果。缺少时区的旧日志统一把已写出的日历字段解释为 UTC，
不再随读取机器的本地时区变化。事件分派及启动、停止、控制信号的直接回调不得重入控制 API；引擎会明确拒绝这类调用，
不会把操作隐式投递到 Qt 事件队列。

## 路由器插件

仿真引擎只管理事件队列、链路、BGP 会话传输和 RIB；每个节点对应一个
`RouterNode` 插件实例。插件控制以下策略点：

- 本地起源路由的创建；
- UPDATE 入站接受、拒绝与属性修改；
- 带属性 WITHDRAW 的入站处理、真实状态撤销属性和按前缀出口属性；
- 候选路由的最佳路径选择；
- 面向每个邻居的出口过滤与属性变换；
- 公共 TFP 属性/per-prefix override 的生成、导入和瞬态因果消费；
- 仿真启动/停止及 Peer 状态通知。

插件也是确定性边界的一部分：策略回调不得读取墙钟、系统熵、线程调度顺序、指针
地址或未排序哈希容器；候选路由顺序由引擎规范为“本地路由优先，其余按区分大小写的
Peer ID 排序”。同一插件二进制和同一输入必须返回相同输出。事件预算可以终止插件不断
产生新事件的振荡，但无法抢占一个永不返回的单次插件回调；插件回调仍必须是有限、同步的计算。

### 添加一个插件

公共接口位于 `src/plugin/RouterPlugin.hpp`。新增插件不需要修改任何 CMake
文件，也不需要生成或复制 DLL：

1. 在 `src/router_plugins/` 下添加同名的 `.hpp` 和 `.cpp` 文件；
2. 在头文件中声明一个实现 `RouterNodePlugin` 的工厂类；
3. `.cpp` 包含 `plugin/RouterPluginRegistry.hpp`，并在文件末尾注册该工厂：

```cpp
BGPTESTER_REGISTER_ROUTER_PLUGIN(my_namespace::MyRouterPlugin)
```

4. 正常执行 `build.ps1` 或 `cmake --build build`；
5. 启动程序，新插件会直接出现在路由器属性的插件列表中。

CMake 使用 `GLOB_RECURSE CONFIGURE_DEPENDS` 自动检测该目录中新加入或删除
的源码。插件源码会直接编译进 BgpTester，不需要命令行参数、环境变量或
额外部署步骤。子目录同样会被自动扫描。

完整的两文件示例见：

- `src/router_plugins/ConfigurableExportRouterPlugin.hpp`；
- `src/router_plugins/ConfigurableExportRouterPlugin.cpp`。

插件 ID 在进程内必须唯一，API 版本当前为 `5`。插件缺失、ID 重复、API
版本不匹配或节点配置校验失败时，程序会给出错误且不会启动该次仿真。
API 5 增加 `convergenceStateChanged(bool)`、`requiresDissemination(prefix)` 与
`decisionCompleted(prefix)` 生命周期钩子，分别用于标记 bootstrap 边界、在经典
`RouteEntry` 未变化时请求一次因果发布，以及在全部 peer 导出后安全消费本轮因果；
不需要这些能力的插件可继续使用默认空实现。

### TFP 路径版本插件

内置源码插件 `org.bgptester.router.tfp-version` 实现路由器级实体版本机制：

- 实验讨论中口语所称的“FTP 路由器”就是该 TFP 插件，不是另一种路由器，也与文件传输协议 FTP 无关；
- 每前缀 `TFP_VERSION_INFO` 分成稳定的 `DependencyVector` 与瞬态的
  `TriggerVector`：Dependency 随候选路径保存，Trigger 只作为当前 UPDATE/WITHDRAW
  的最小因果 delta，接收后从稳定路由剥离；
- bootstrap（启动至第一次完全收敛）只建立基线依赖，首轮不发送 Trigger；
  第一次稳定后，`LocalVer` 只在标准 BGP 投影中的最佳路由/发布状态或真实本地
  起源状态改变时推进，单纯接收、去重或消费 TFP 元数据不会推进版本；
- 每个前缀按 `(ASN, EntityID)` 独立维护本地 `MaxVer`。它吸收收到的 Dependency
  与 Trigger 的逐实体最大值，但只用于本地 stale 判断，绝不会整表作为 Trigger 外发；
- `MaxVer` 使用实体哈希索引；故障期的 stale 前沿与 Trigger 去重水位合并在同一个
  `TriggerKnowledge` 哈希条目中，避免为相同实体维护两套键和节点；在尚未观察到已知实体版本推进时，
  bootstrap 选路走经典 BGP 快路径，不反复扫描不可能过期的 Dependency。经典路由
  等价比较直接忽略 TFP 字段，不复制并清空整组候选向量；
- 本地 `MaxVer` 不物化无信息增益的版本 `0`，空表使用 const 探测以保持零分配；线路与稳定路由仍保留显式 `(Entity,0)` Dependency，首次正版本会从隐式零基线推进 stale frontier，`initial_version > 0` 也会启用本地旧依赖检查；
- stale 判定只遍历真正推进过版本的稀疏 `StaleFrontier`，再到候选 Dependency 中
  查找对应实体；不再为每个候选遍历整条 Dependency 并反复哈希查询完整 `MaxVer`；
- TFP 在原候选数组上完成 stale 过滤和标准 BGP primary/stickiness/tie-break 扫描，不再构造临时 `filteredCandidates`；常见单 Trigger 通过隐式共享复用入站或本地产生的 canonical 小向量；
- Trigger 按 `(prefix, Entity, Version)` 去重。首次看到的更高 Trigger 进入待转发
  因果，即使经典 best 未变，也会携带当前 selected route，沿标准出口策略允许的
  邻居转发一次最小 delta；它不推进 `LocalVer`，也不是绕过策略的全邻居泛洪。
  全部 peer 导出完成后消费本轮因果，重复或较旧 Trigger 不再产生 UPDATE；
- pending 或已 flush、尚未交付的消息被新 generation 覆盖时，非空 Trigger 会迁移到最新状态；引擎只按单次仿真内唯一的 generation 建立稀疏 `generation -> TriggerVector` 保真索引，并在交付、取消、邻居断开或停止时删除，不把向量内联到数百万个普通 outbound generation；
- 同一 decision 向多个策略允许的 peer 导出时，只构造一次“所选路径 Dependency + 本地实体版本”，各 peer 隐式共享该稳定向量；AS_PATH、NEXT_HOP、RR 和出口策略仍逐 peer 处理，临时缓存由 `decisionCompleted` 清理；
- 若入站 Trigger 已显式证明旧 best 的 Dependency 过期，由此产生的本地经典变化
  仍推进 `LocalVer`，但出站只转发该根因，不再逐跳追加冗余的本地 Trigger；新的
  Dependency 仍携带推进后的本地版本。没有可证明入站根因的真实本地变化照常发布
  本地 Trigger；
- 只有显式 Dependency 落后于本地 `MaxVer` 的候选路径会被排除；缺少版本信息的
  普通 BGP 路径不会被当作版本 `0`；
- TFP 信息逻辑上按前缀隔离；相同值使用一份公共属性，不同值使用 sidecar override。
  标准共享路径属性相同的多个 NLRI 仍保持一条 UPDATE/WITHDRAW，前缀过滤和 generation guard 会同步处理其元数据；
- 纯 Dependency 或其他 TFP 元数据刷新不会推进 `LocalVer`、也不会凭空生成 Trigger；若选中路由的持久 Dependency 发生变化，可随普通聚合 UPDATE 刷新下游路径证明；
- EBGP、IBGP 与 Route Reflector 继续使用标准插件的传播/选路规则；
- 仅由出口过滤或 split-horizon 产生的策略撤销不携带版本信息；
- 64 位本地版本在同一次仿真及节点关闭/恢复期间保持单调，达到上限后饱和且不回绕；
  `initial_version` 可为十进制字符串，用来衔接外部持久化版本。部署方应在耗尽前轮换实体标识或迁移持久化基线。

插件配置示例：

```json
"plugin": {
  "id": "org.bgptester.router.tfp-version",
  "settings": {
    "entity_id": "border-router-1",
    "initial_version": "1000"
  }
}
```

`entity_id` 为空时使用 Router ID。稳定 RIB 与快照只保留 Dependency；Trigger
只出现在仍携带该瞬态因果的报文记录中。Dependency-only bootstrap 报文不额外写入
逐报文 TFP 明细；小拓扑携带 Trigger 的报文记录摘要，且对不超过 4 项的单前缀
Trigger 保留可读向量。大型拓扑只保留聚合报文和固定间隔样本的摘要，不展开逐项
向量，避免诊断日志和 SQLite 背压参与收敛时间。

以下数据是墙钟驱动版本 R29 的历史性能基线，不可与当前纯离散事件引擎直接比较。
固定 `misc/caida_topology_layout.json` 的最终 R29 同构建 headless 对照（4005 路由器、5464 链路、
1000 前缀，断开 `AS141733_C1`—`AS2764_C3`）中，标准与 TFP 的 bootstrap protocol 均为
219334 ms、报文均为 5398928，active wall 为 219342/219343 ms，wall confirm 为 220363/220346 ms。
故障后标准的 protocol/active wall/wall confirm 为 197/256/1300 ms，TFP 为 154/208/1263 ms；
标准为 6785 报文（UPDATE 1047、WITHDRAW 5738），TFP 为 5387 报文（UPDATE 0、WITHDRAW 5387）。
即 TFP 故障 protocol、active wall、wall confirm 和报文分别减少 21.83%、18.75%、2.85% 和 20.60%。
TFP bootstrap 峰值工作集为 8.7135 GiB，较优化前 R28 的 15.7753 GiB 下降 44.77%。这里的 UPDATE
数量是路径探索报文代理，不是直接统计的 Loc-RIB 最佳路由切换次数。两组最终逐路由
`best_route_count` 映射及目标前缀可达性一致；映射按 Unicode ordinal 排序后，以 UTF-8、LF、无末尾换行的
`id=best_route_count` 序列计算，SHA-256 均为 `29a3aa96b3684c52a9ecc5d617d0fa6227b4f1df2a6b3a5deabcad7d332550f3`。
本轮未逐项比较全部 prefix/path/next-hop，因而不据此声明完整 RIB 等价。
完整实验记录见仓库工作区 `tmp/caida_experiment_summary.md`。

### 日志

每次运行在 `<log_dir>/<实验名>_<时间>/` 下生成：

- `bmp_collector.log`：JSON Lines；
- `bmp_collector.sqlite`：带常用查询索引的 SQLite 历史库。

窗口底部的 BMP 监控器提供“事件日志”和“收敛时间”两个页面。事件日志页可通过“显示列”按钮或表头右键菜单选择要显示、隐藏的列，选择会在下次启动时恢复。收敛时间页实时显示当前状态，并为每轮分别显示协议活动时间和包含静默窗口的仿真确认时间；二者都来自确定性的虚拟时钟，不包含机器或日志耗时。每轮 BGP 报文数随 `converged` 事件持久化，因此重新打开日志仍可查看；旧日志未保存该字段时显示“—”。双击事件记录可查看完整 JSON；“打开历史”会在后台查询 SQLite，只把最近 20000 条匹配事件放入表格，并另外显示完整日志中的事件总数、过滤后事件数、BGP 报文总数和过滤后报文数。这里的“报文”严格指 `event=message_received`，不包含拓扑和收敛事件；一条包含多个前缀的聚合 UPDATE/WITHDRAW 仍只计一条报文。

事件持久化运行在独立线程中。仿真线程先把批量事件放入有上限的队列，后台线程批量写入 JSONL 和 SQLite；队列达到上限时会对仿真施加背压，避免以内存无限堆积换取吞吐。GUI 每帧只抽取一批最近事件，实时表最多保留 20000 条，完整历史始终保存在日志文件中。

## JSON

程序使用 `simulation`、`routers`、`links` 三段结构。链路中的方向字段以 `a` / `b` 为基准：

- `simulation.withdrawal_ignores_mrai`：全局布尔开关；`true` 表示 WITHDRAW 绕过 MRAI，`false` 表示 WITHDRAW 与 UPDATE 一同受每邻居 MRAI 约束。字段缺失时默认为 `true`；
- `rr_client_from_a`：A 把 B 配置为 RR Client；
- `mrai_ms_from_a`：A 向 B 发送路由变更的 MRAI；
- `rr_client_from_b` / `mrai_ms_from_b`：反方向。
- `relationship`：eBGP 商业关系，可取 `unspecified`、`peer`、`a_provider` 或 `b_provider`；`a_provider` 表示 A 是 B 的 provider，`b_provider` 表示 B 是 A 的 provider。

新保存的拓扑只以 `links` 作为链路与会话配置的事实来源，不再生成重复的 `neighbors` 数组。加载器仍兼容旧拓扑中的 `neighbors`：它可补充 `links` 中缺失的方向性 MRAI/RR、启用状态和商业关系，也可在没有对应 `links` 条目时补建链路；同一字段同时出现时以 `links` 为准。

旧 `neighbors[].relationship` 使用本地路由器视角，可取 `unspecified`、`peer`、`provider` 或 `customer`。当 `links` 未显式给出 `enabled` 时，双向旧邻居条目的 `enabled` 按逻辑 AND 合并，即任一端为 `false` 时链路停用；双向 `relationship` 换算到链路视角后若互相矛盾，加载器会拒绝该拓扑。旧拓扑缺少商业关系字段时按 `unspecified` 处理，以保留原有普通 eBGP 行为。

每台路由器可使用独立插件；旧拓扑未提供 `plugin` 时自动使用内置标准
BGP 插件：

```json
"plugin": {
  "id": "org.bgptester.router.standard-bgp",
  "settings": {}
}
```

示例见 `topo/sample_topology.json`。
