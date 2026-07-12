# BGP 网络测试框架

当前实现是 [`BgpTester/`](BgpTester/)：一个使用 C++20 与 Qt 6 编写的统一桌面程序。拓扑编辑、BGP 仿真、运行时控制和日志查看均位于同一个进程、工程与窗口中。

## BgpTester 功能

- 可视化添加、编辑、拖动和删除路由器与链路；
- AS 分组框、RR 标记、方向性 RR Client/MRAI 与链路状态可视化；
- JSON 拓扑加载、保存与旧拓扑方向字段读取；
- 节点级路由器源码插件、自动发现与插件私有 JSON 配置；
- 事件驱动 BGP OPEN、UPDATE、NOTIFICATION 仿真；
- Adj-RIB-In、Loc-RIB、Adj-RIB-Out、EBGP、IBGP 和基础 Route Reflector；
- LOCAL_PREF、AS_PATH、MED、会话类型、旧路径稳定性与确定性 tie-break 选路；
- 每邻居 MRAI、FRR 风格 withdrawal 聚合、链路延迟和逐前缀 stale UPDATE generation guard；
- 运行时节点/链路上下线、前缀发布/撤销与收敛状态；
- 最佳路由、全部路径、Peer 状态检查以及画布逐跳路径高亮；
- BMP 风格实时事件表、全列过滤、JSON 详情、JSONL 和 SQLite 历史日志；
- 核心回归测试，覆盖 JSON、校验、EBGP、RR、MRAI、源码插件与日志持久化。

## 构建

程序只依赖用户自行安装的 Qt 6 与 C++ 工具链，不使用 vcpkg，也不会自动下载依赖。

```powershell
cd BgpTester
.\build.ps1
```

生成可分发目录（调用 Qt 自带 `windeployqt`）：

```powershell
.\build.ps1 -Configuration Release -Deploy
```

详细依赖、界面操作、仿真语义和 JSON 格式见 [`BgpTester/README.md`](BgpTester/README.md)。

## 目录

```text
.
├─ BgpTester/       # 当前 Qt 6 / C++20 统一实现
└─ misc/            # RFC 与实验辅助文件
```
