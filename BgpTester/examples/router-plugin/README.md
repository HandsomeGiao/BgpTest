# 自定义路由器插件示例

这个示例实现 `org.bgptester.example.configurable-export`，演示节点级 JSON
配置、自定义选路、入站处理和出口过滤。

在本目录构建：

```powershell
cmake -S . -B build `
  -DCMAKE_PREFIX_PATH="C:\path\to\Qt\6.x.x\mingw_64" `
  -DBGPTESTER_SDK_DIR="D:\path\to\BgpTester"
cmake --build build --config Release
```

插件必须使用与 BgpTester 相同的 Qt、编译器、架构和 C++ ABI。把生成的
动态库复制到 `BgpTester.exe` 旁的 `plugins/routers/`，或通过
`--router-plugin-dir <目录>` / `BGPTESTER_ROUTER_PLUGIN_PATH` 加载。

节点配置示例：

```json
"plugin": {
  "id": "org.bgptester.example.configurable-export",
  "settings": {
    "export_routes": true,
    "local_preference": 250
  }
}
```
