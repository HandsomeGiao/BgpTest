# BgpTester

`BgpTester` 是统一完成拓扑编辑与 BGP 测试的 Qt 6 / C++20 桌面程序。拓扑编辑、BGP 仿真、运行时扰动、RIB/Peer 检查、路径高亮和 BMP 风格日志浏览都位于同一窗口。

## 依赖

- CMake 3.25 或更新版本
- 支持 C++20 的编译器
- Qt 6.5 或更新版本，需安装 `Core`、`Gui`、`Widgets`、`Network`、`Sql` 模块和 SQLite driver
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
- `Q` 或“添加链路”：依次单击两台路由器，设置链路延迟、两个方向的 MRAI 与 RR Client 关系。
- `V` 返回选择模式；拖动路由器可调整布局，右键/中键拖动画布，滚轮缩放。
- 双击路由器或链路可编辑；Delete 删除所选对象。
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
- 选路顺序：本地起源、LOCAL_PREF、AS_PATH 长度、MED、EBGP 优先、旧路稳定性、确定性 tie-break；
- 每邻居 MRAI，withdrawal 立即发送；延迟队列使用 generation guard，旧 UPDATE 不会复活已撤销路由；
- 链路延迟、节点/链路运行时状态以及静默窗口收敛判定。

### 日志

每次运行在 `<log_dir>/<实验名>_<时间>/` 下生成：

- `bmp_collector.log`：JSON Lines；
- `bmp_collector.sqlite`：带常用查询索引的 SQLite 历史库。

窗口底部提供实时事件表、全列过滤、排序和跟随滚动。双击记录可查看完整 JSON；“打开历史”可载入已有 SQLite 日志。

## JSON

程序使用 `simulation`、`routers`、`links` 三段结构。链路中的方向字段以 `a` / `b` 为基准：

- `rr_client_from_a`：A 把 B 配置为 RR Client；
- `mrai_ms_from_a`：A 向 B 发送 UPDATE 的 MRAI；
- `rr_client_from_b` / `mrai_ms_from_b`：反方向。

保存时会同时生成每台路由器的 `neighbors` 数组，便于人工阅读。加载旧拓扑时也会读取其中的方向性 MRAI/RR 字段。

示例见 `topo/sample_topology.json`。
