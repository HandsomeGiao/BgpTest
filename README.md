# BGP 网络测试框架

本项目是一个面向大规模 BGP 网络收敛测试的实验框架。它由两个相互隔离的子项目组成：

- `TopoSimulator/`：纯 C++20 实现的 BGP 拓扑模拟器，负责读取拓扑 JSON、启动路由器节点、模拟 BGP 报文传播、记录 BMP 风格日志、打开实时观察窗口，并支持运行时拓扑扰动。
- `TopoGenerator/`：PyQt6 实现的可视化拓扑生成器，负责通过图形界面创建路由器、链路和 BGP 邻居关系，并导出模拟器可读取的 JSON 文件。

项目目标是提供一个足够灵活的 BGP 网络协议测试底座，让使用者可以在基础 BGP 行为之上扩展自定义协议、策略和收敛逻辑。

## 功能概览

- 从 JSON 文件读取网络拓扑、路由器配置、BGP 邻居关系和链路状态。
- 邻居连接是模拟器内存里的消息投递，不使用真实 socket 或接口 IP；`router_id` 是 BGP router-id，用于协议标识和 NEXT_HOP 字段。
- 模拟 BGP OPEN、UPDATE、NOTIFICATION 等基础报文；为减少仿真报文量，当前不生成 KEEPALIVE，邻居在线状态由拓扑 link/node 状态控制。
- 维护 Adj-RIB-In、Loc-RIB、Adj-RIB-Out 等核心 BGP 路由信息结构。
- 支持 IBGP、EBGP 和基础路由反射器行为；默认选路按 local-pref、AS_PATH 长度、MED、EBGP/IBGP、旧路径优先进行比较，只有当前没有同优旧路径时才使用 next-hop 和来源邻居作为确定性 tie-breaker。
- 支持通过拓扑 JSON 的 `simulation.router_class` 或启动参数 `--router-class` 为每次实验选择不同的 C++ 路由器类，便于测试自定义 BGP 行为。
- 支持邻居级 MRAI，用于控制同一路由器向同一邻居发送广告 UPDATE 的最小间隔；withdrawal-only UPDATE 会绕过 MRAI 立即发送。
- 同一邻居的 MRAI pending 广告 UPDATE 会在下一个 MRAI 时刻作为一次传输批量 flush；同一轮产生的多个 withdrawal-only UPDATE 也会立即作为一次传输批量发送。BMP 仍会逐条记录批次内的逻辑 UPDATE/WITHDRAW；接收端会先导入整批报文，再对所有受影响前缀统一运行一次选路并向外扩散。如果到点时全部失效，则不发送报文，也不额外占用下一次 MRAI 发送机会。
- 收敛判定会等待工作队列持续空闲至少 `max(convergence_quiet_ms, 1000ms)`；MRAI 定时任务会保留在线程池中，因此不会被额外静默窗口吞掉。
- 使用 C++ 多线程加速报文投递和拓扑模拟。
- 启动时会校验拓扑配置，提前拒绝重复路由器、重复链路、自连接链路和未知端点。
- 生成 `bmp_collector.log` 和 `bmp_collector.sqlite`，分别用于 JSON Lines 原始记录和 SQLite 历史查询；BMP 时间戳使用 `YYYY-MM-DD HH:MM:SS.mmm` 格式的中国时间。
- 交互式启动时会自动打开 ImGui BMP 日志窗口，用于观察收敛过程、自定义表格显示列，并通过 `MessageFilter` 按相关路由器、来源/目的路由器、动作类型、来源 AS 和目的 AS 查询历史或实时过滤报文；也会构建独立的 `BmpLogViewer.exe`，用于打开已有 `bmp_collector.sqlite` 做离线分析；withdraw-only UPDATE 会直接标识为 `WITHDRAW`。
- 支持交互式运行时操作，例如断开链路、恢复链路、关闭节点、恢复节点、发布或撤销前缀。
- 提供 PyQt 可视化拓扑编辑器，用于生成模拟器输入 JSON；导入已有拓扑后会保留链路方向上的 MRAI 和 RR client 设置，并在下次启动时自动恢复最近加载或导出的拓扑。
- 提供 CTest 测试目标，覆盖核心拓扑校验、MRAI 广告、立即撤销、同邻居多前缀批量 flush、批量接收后统一选路和 stale flush 不占用后续发送机会等场景。

## 架构

```text
BGP Test Framework
├─ TopoGenerator
│  ├─ PyQt6 UI
│  ├─ Router / Link topology model
│  └─ JSON exporter
│
└─ TopoSimulator
   ├─ CLI entry
   ├─ TopoManager
   ├─ BgpRouter
   ├─ RouterFactory
   ├─ ThreadPool
   ├─ BmpLogManager
   ├─ BmpLogViewer / BmpLogViewer.exe
   └─ BGP message / route data model
```

### TopoSimulator

`TopoSimulator` 是核心仿真引擎，只支持 Windows + MSVC + vcpkg，使用 C++20、CMake 和 vcpkg manifest 构建。

依赖边界规则：

- 核心模拟层只能使用纯 C++20 标准库和项目自身头文件，包括 `BgpRouter`、`RouterFactory`、`TopoManager`、`ThreadPool`、`BgpTypes` 等路由器节点、BGP 管理器、协议状态机和决策逻辑。
- 协议状态机和路由决策保持纯 C++20；第三方库和 Windows API 仅用于外围设施，例如命令行交互、颜色输出、JSON 拓扑读取、BMP 日志持久化和可视化窗口。
- 如果需要在核心层暴露运行状态，应返回标准库数据结构；由外围适配层负责转换成 JSON、日志文本或控制台输出。

主要模块：

- `BgpRouter`：表示一个 BGP 路由器节点，维护邻居状态、RIB 数据结构和基础 BGP 行为。该类保留了多个虚函数扩展点，便于子类重写接收报文、导入策略、导出策略、路径选择和属性转换逻辑。
- `RouterFactory`：维护可用路由器类注册表，根据 `simulation.router_class` 或 `--router-class` 为本次实验创建对应的路由器对象。
- `TopoManager`：管理整个拓扑的生命周期，负责启动、停止、链路状态变化、节点状态变化，以及路由器之间的报文转发。
- `TopoManager` 会在延迟报文真正投递前重新检查链路和节点状态，避免链路或节点已关闭后仍投递旧报文。
- `ThreadPool`：提供多线程任务执行能力，用于并发模拟报文投递和节点处理。
- `BmpLogManager`：模拟 BMP collector，把 BGP 节点收到的报文和拓扑事件写入 JSON Lines 与 SQLite，并维护实时窗口使用的内存缓冲。
- `BmpLogViewer`：基于 ImGui 的轻量窗口，用于实时观察收敛过程和查询分析历史 BMP 报文。
- `BgpTypes`：定义 BGP 消息、路径属性、邻居配置、路由条目和拓扑配置等数据结构。

默认无参数启动时，模拟器只会检查 `TopoSimulator.exe` 所在目录下的 `topo/` 文件夹，并列出其中可用的 `*.json` 拓扑文件。如果没有可用拓扑，会提示后退出。

详细构建和运行说明见 [TopoSimulator/README.md](TopoSimulator/README.md)。

### TopoGenerator

`TopoGenerator` 是独立的 Python/PyQt6 项目，不与 C++ 模拟器代码混杂。

它用于：

- 添加、编辑和删除路由器节点。
- 配置 router id、ASN、cluster id 和本地起源前缀。
- 添加和编辑节点之间的链路。
- 配置链路状态、链路延迟、双向 MRAI 和 RR client 方向。
- 新增路由器默认生成可读且合法的 BGP router-id，例如 `10.0.0.1`、`10.0.0.2`、`10.0.1.1`，避免大规模拓扑中出现 `300.300.300.300` 这类非法值。
- 加载已有模拟器 JSON 时，会从邻居配置恢复双向 MRAI 和 RR client 方向，避免 round-trip 导出时丢失协议配置。
- 导出 `TopoSimulator` 可读取的 JSON 拓扑文件。

详细使用说明见 [TopoGenerator/README.md](TopoGenerator/README.md)。

## 运行流程

1. 使用 `TopoGenerator` 创建网络拓扑，或手写 JSON 拓扑文件。
2. 将拓扑 JSON 放入 `TopoSimulator.exe` 所在目录的 `topo/` 文件夹，或使用 `--topology` 参数显式指定文件。
3. 启动 `TopoSimulator`。
4. 模拟器读取拓扑，并从 `--router-class`、`simulation.router_class` 或交互式启动选择中确定本次实验使用的路由器类。
5. 模拟器创建路由器节点和 BGP 邻居关系。
6. 网络开始交换 BGP 报文并收敛。
7. 模拟器生成 `tmp/<run>/bmp_collector.log` 和 `tmp/<run>/bmp_collector.sqlite`，记录 BGP 报文和拓扑事件。
8. 交互式运行时自动打开 BMP 日志窗口；批处理或测试场景可使用 `--bmp-viewer off` 禁用窗口。窗口被关闭后，可在 CLI 中执行 `bmp viewer` 或 `bmp open` 重新打开。
9. 收敛后进入交互模式，可继续执行链路断开、节点关闭、路由发布或撤销等操作。

## 目录结构

```text
.
├─ README.md
├─ TopoSimulator/
│  ├─ CMakeLists.txt
│  ├─ vcpkg.json
│  ├─ build.ps1
│  ├─ include/toposim/
│  ├─ src/
│  ├─ tests/
│  ├─ config/
│  └─ topo/
├─ TopoGenerator/
│  ├─ pyproject.toml
│  ├─ requirements.txt
│  └─ topogenerator/
└─ misc/
   └─ rfc4271.txt
```

## 快速开始

构建模拟器：

```powershell
.\TopoSimulator\build.ps1
```

运行模拟器测试：

```powershell
ctest --test-dir TopoSimulator\build -C Release --output-on-failure
```

运行模拟器：

```powershell
.\TopoSimulator\build\Release\TopoSimulator.exe
```

显式控制 BMP 日志窗口：

```powershell
.\TopoSimulator\build\Release\TopoSimulator.exe --topology TopoSimulator\topo\sample_topology.json --bmp-viewer auto
.\TopoSimulator\build\Release\TopoSimulator.exe --topology TopoSimulator\topo\sample_topology.json --bmp-viewer off
```

运行中可用以下命令控制 BMP 日志窗口：

```text
bmp viewer
bmp open
bmp close
bmp status
```

离线分析已有 SQLite 日志：

```powershell
.\TopoSimulator\build\Release\BmpLogViewer.exe path\to\bmp_collector.sqlite
```

不传路径时，`BmpLogViewer.exe` 会弹出文件选择窗口。

运行拓扑生成器：

```powershell
cd TopoGenerator
python -m pip install -r requirements.txt
python -m topogenerator.main
```

## 扩展自定义 BGP 协议

自定义协议逻辑的主要入口是继承 `toposim::BgpRouter`。推荐保持 `TopoManager` 负责拓扑生命周期、链路/节点状态、消息投递和日志系统，只替换具体路由器的策略行为。

常用步骤：

1. 新增一个 `BgpRouter` 子类，例如 `LabRouter`。
2. 按需要重写导入策略、导出策略、属性转换、选路规则或消息处理函数。
3. 如果新增 `.cpp` 文件，把它加入 `TopoSimulator/CMakeLists.txt` 的 `toposim` target。
4. 在自定义路由器的 `.cpp` 中调用 `registerRouterClass("LabRouter", ...)` 注册类名。
5. 在拓扑 JSON 中设置 `simulation.router_class`，或启动时使用 `--router-class LabRouter` 覆盖本次实验的路由器类。

常用虚函数：

- `onMessageReceived`
- `onOpenMessage`
- `onUpdateMessage`
- `onNotificationMessage`
- `importRouteAllowed`
- `exportRouteAllowed`
- `transformRouteForPeer`
- `selectBestRoute`

如果重写消息处理函数并仍希望保留默认 RIB / 状态机副作用，应调用对应的 `BgpRouter::on...` 基类实现。如果只是调整策略，通常优先覆盖 `importRouteAllowed`、`exportRouteAllowed`、`transformRouteForPeer` 和 `selectBestRoute`。

示例接入方式：

```cpp
#include "toposim/LabRouter.hpp"
#include "toposim/RouterFactory.hpp"

namespace {

const bool registered_lab_router = [] {
  toposim::registerRouterClass(
      "LabRouter", [](toposim::RouterConfig config) {
        return std::make_shared<toposim::LabRouter>(std::move(config));
      });
  return true;
}();

} // namespace
```

```json
{
  "simulation": {
    "name": "lab",
    "router_class": "LabRouter"
  }
}
```

也可以只覆盖一次启动：

```powershell
.\TopoSimulator.exe --topology testtopology.json --router-class LabRouter
```

更完整的示例和注意事项见 [TopoSimulator/README.md](TopoSimulator/README.md) 的 `Custom BGP Router Behavior` 章节。
