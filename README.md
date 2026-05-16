# BGP 网络测试框架

本项目是一个面向大规模 BGP 网络收敛测试的实验框架。它由两个相互隔离的子项目组成：

- `TopoSimulator/`：纯 C++20 实现的 BGP 拓扑模拟器，负责读取拓扑 JSON、启动路由器节点、模拟 BGP 报文传播、记录 BMP 风格日志，并支持运行时拓扑扰动。
- `TopoGenerator/`：PyQt6 实现的可视化拓扑生成器，负责通过图形界面创建路由器、链路和 BGP 邻居关系，并导出模拟器可读取的 JSON 文件。

项目目标是提供一个足够灵活的 BGP 网络协议测试底座，让使用者可以在基础 BGP 行为之上扩展自定义协议、策略和收敛逻辑。

## 功能概览

- 从 JSON 文件读取网络拓扑、路由器配置、BGP 邻居关系和链路状态。
- 邻居连接是模拟器内存里的消息投递，不使用真实 socket 或接口 IP；`router_id` 是 BGP router-id，用于协议标识和 NEXT_HOP 字段。
- 模拟 BGP OPEN、KEEPALIVE、UPDATE、NOTIFICATION 等基础报文。
- 维护 Adj-RIB-In、Loc-RIB、Adj-RIB-Out 等核心 BGP 路由信息结构。
- 支持 IBGP、EBGP 和基础路由反射器行为。
- 支持邻居级 MRAI，用于控制同一前缀向同一邻居重复广告的最小间隔。
- MRAI 延迟广告会在投递前检查是否仍然有效，避免已撤销前缀被旧 UPDATE 重新传播。
- 收敛判定会等待至少 `1.5 * 最大 MRAI` 的静默窗口，让 MRAI 场景下的判断更稳妥。
- 使用 C++ 多线程加速报文投递和拓扑模拟。
- 启动时会校验拓扑配置，提前拒绝重复路由器、重复链路、自连接链路和未知端点。
- 生成 `bmp_collector.log`，以 JSON Lines 形式记录所有节点收到的 BGP 报文。
- 支持交互式运行时操作，例如断开链路、恢复链路、关闭节点、恢复节点、发布或撤销前缀。
- 提供 PyQt 可视化拓扑编辑器，用于生成模拟器输入 JSON；导入已有拓扑后会保留链路方向上的 MRAI 和 RR client 设置。
- 提供 CTest 测试目标，覆盖核心拓扑校验和 MRAI 撤销场景。

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
   ├─ ThreadPool
   ├─ BmpCollector
   └─ BGP message / route data model
```

### TopoSimulator

`TopoSimulator` 是核心仿真引擎，只支持 Windows + MSVC + vcpkg，使用 C++20、CMake 和 vcpkg manifest 构建。

依赖边界规则：

- 核心模拟层只能使用纯 C++20 标准库和项目自身头文件，包括 `BgpRouter`、`TopoManager`、`ThreadPool`、`BgpTypes` 等路由器节点、BGP 管理器、协议状态机和决策逻辑。
- 第三方库和 Windows API 只能用于外围设施，例如命令行交互、颜色输出、日志输入输出、JSON 拓扑读取和 JSON 格式化展示。
- 如果需要在核心层暴露运行状态，应返回标准库数据结构；由外围适配层负责转换成 JSON、日志文本或控制台输出。

主要模块：

- `BgpRouter`：表示一个 BGP 路由器节点，维护邻居状态、RIB 数据结构和基础 BGP 行为。该类保留了多个虚函数扩展点，便于子类重写接收报文、导入策略、导出策略、路径选择和属性转换逻辑。
- `TopoManager`：管理整个拓扑的生命周期，负责启动、停止、链路状态变化、节点状态变化，以及路由器之间的报文转发。
- `TopoManager` 会在延迟报文真正投递前重新检查链路和节点状态，避免链路或节点已关闭后仍投递旧报文。
- `ThreadPool`：提供多线程任务执行能力，用于并发模拟报文投递和节点处理。
- `BmpCollector`：模拟 BMP collector，把 BGP 节点收到的报文和拓扑事件写入日志。
- `BgpTypes`：定义 BGP 消息、路径属性、邻居配置、路由条目和拓扑配置等数据结构。

默认无参数启动时，模拟器只会检查当前工作目录下的 `topo/` 文件夹，并列出其中可用的 `*.json` 拓扑文件。如果没有可用拓扑，会提示后退出。

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
2. 将拓扑 JSON 放入模拟器当前工作目录的 `topo/` 文件夹，或使用 `--topology` 参数显式指定文件。
3. 启动 `TopoSimulator`。
4. 模拟器读取拓扑，创建路由器节点和 BGP 邻居关系。
5. 网络开始交换 BGP 报文并收敛。
6. 模拟器生成 `tmp/<run>/bmp_collector.log`，记录 BGP 报文和拓扑事件。
7. 收敛后进入交互模式，可继续执行链路断开、节点关闭、路由发布或撤销等操作。

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

运行拓扑生成器：

```powershell
cd TopoGenerator
python -m pip install -r requirements.txt
python -m topogenerator.main
```

## 扩展自定义 BGP 协议

自定义协议逻辑的主要入口是继承 `toposim::BgpRouter`，并按需要重写以下虚函数：

- `onMessageReceived`
- `onOpenMessage`
- `onKeepaliveMessage`
- `onUpdateMessage`
- `onNotificationMessage`
- `importRouteAllowed`
- `exportRouteAllowed`
- `transformRouteForPeer`
- `selectBestRoute`

这样可以在不重写拓扑管理、多线程投递和日志系统的前提下，替换 BGP 策略、路径选择、属性处理或自定义报文行为。
