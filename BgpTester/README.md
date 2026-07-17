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
- 同一轮中路径属性相同的待发布前缀全部聚合到一条 UPDATE，不限制 NLRI 数量；
- withdrawal 不等待 MRAI，并将本轮待撤销前缀全部聚合到一条报文；generation guard 按前缀过滤，不会误丢同批其他路由；
- 链路延迟、节点/链路运行时状态以及静默窗口收敛判定。

## 路由器插件

仿真引擎只管理事件队列、链路、BGP 会话传输和 RIB；每个节点对应一个
`RouterNode` 插件实例。插件控制以下策略点：

- 本地起源路由的创建；
- UPDATE 入站接受、拒绝与属性修改；
- 带属性 WITHDRAW 的入站处理、真实状态撤销属性和按前缀出口属性；
- 候选路由的最佳路径选择；
- 面向每个邻居的出口过滤与属性变换；
- 仿真启动/停止及 Peer 状态通知。

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

插件 ID 在进程内必须唯一，API 版本当前为 `4`。插件缺失、ID 重复、API
版本不匹配或节点配置校验失败时，程序会给出错误且不会启动该次仿真。

### TFP 路径版本插件

内置源码插件 `org.bgptester.router.tfp-version` 实现路由器级实体版本机制：

- `TFP_VERSION_INFO` 作为显式路径属性保存 `DependencyVector` 与
  `TriggerVector`，UPDATE 和真实路径撤销均可携带；
- 每个前缀按 `(ASN, EntityID)` 独立维护最大已知版本，只排除显式依赖较旧
  版本的候选路径；缺少版本信息的普通 BGP 路径不会被当作版本 `0`；
- EBGP、IBGP 与 Route Reflector 继续使用标准插件的传播/选路规则；
- 仅由出口过滤或 split-horizon 产生的策略撤销不携带版本信息；
- 64 位本地版本在同一次仿真及节点关闭/恢复期间保持单调，
  `initial_version` 可为十进制字符串，用来衔接外部持久化版本。

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

`entity_id` 为空时使用 Router ID。消息事件详情会记录
`tfp_dependency_vector` 与 `tfp_trigger_vector`，便于在实时日志、JSONL 和
SQLite 历史中检查传播过程。

### 日志

每次运行在 `<log_dir>/<实验名>_<时间>/` 下生成：

- `bmp_collector.log`：JSON Lines；
- `bmp_collector.sqlite`：带常用查询索引的 SQLite 历史库。

窗口底部的 BMP 监控器提供“事件日志”和“收敛时间”两个页面。事件日志页可通过“显示列”按钮或表头右键菜单选择要显示、隐藏的列，选择会在下次启动时恢复。收敛时间页实时显示当前收敛过程的触发事件与已用时，并保留本次运行中每一轮收敛的触发事件、开始时间、完成时间和持续时间；持续时间包含配置的收敛静默窗口。双击事件记录可查看完整 JSON；“打开历史”会在后台查询 SQLite，只把最近 20000 条匹配事件放入表格，并另外显示完整日志中的事件总数、过滤后事件数、BGP 报文总数和过滤后报文数。这里的“报文”严格指 `event=message_received`，不包含拓扑和收敛事件。

事件持久化运行在独立线程中。仿真线程先把批量事件放入有上限的队列，后台线程批量写入 JSONL 和 SQLite；队列达到上限时会对仿真施加背压，避免以内存无限堆积换取吞吐。GUI 每帧只抽取一批最近事件，实时表最多保留 20000 条，完整历史始终保存在日志文件中。

## JSON

程序使用 `simulation`、`routers`、`links` 三段结构。链路中的方向字段以 `a` / `b` 为基准：

- `rr_client_from_a`：A 把 B 配置为 RR Client；
- `mrai_ms_from_a`：A 向 B 发送 UPDATE 的 MRAI；
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
