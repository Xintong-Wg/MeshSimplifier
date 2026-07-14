# MeshForge 调试和热重载指南

## 快速开始

### 1. 构建 Debug 版本

```bash
# 使用脚本构建
./scripts/build_debug.sh

# 或手动构建
cmake -S . -B build_debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build_debug -j 8
```

### 2. 运行

```bash
./build_debug/bin/MeshForgeApp
```

### 3. 使用 VSCode 调试

手动修改 `.vscode/launch.json`（注意：该目录文件需要用户编辑）：

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Debug (Launch)",
            "type": "lldb",
            "request": "launch",
            "program": "${workspaceFolder}/build_debug/bin/MeshForgeApp",
            "args": [],
            "cwd": "${workspaceFolder}",
            "preLaunchTask": "Build (Debug)",
            "env": {
                "DYLD_LIBRARY_PATH": "${workspaceFolder}/build_debug/lib"
            },
            "internalConsoleOptions": "openOnSessionStart"
        }
    ]
}
```

创建 `.vscode/tasks.json`：

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "Build (Debug)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build",
                "build_debug",
                "-j",
                "8"
            ],
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "problemMatcher": ["$gcc"]
        }
    ]
}
```

然后按 `F5` 开始调试。

## 可用脚本

| 脚本 | 功能 |
|------|------|
| `./scripts/build_debug.sh` | 构建 Debug 版本 |
| `./scripts/build_release.sh` | 构建 Release 版本 |
| `./scripts/clean.sh` | 清理构建目录 |
| `./scripts/watch_build.sh` | 监听文件变化自动重新构建 |

## 热重载

### 自动重新构建（热重载）

使用 `watch_build.sh` 监听文件变化：

```bash
# 安装依赖
brew install fswatch

# 启动监听
./scripts/watch_build.sh
```

当 `src/` 目录或 `CMakeLists.txt` 文件变化时，会自动重新构建。

### C++ 热重载（进阶）

对于真正的 C++ 代码热重载，你可以：

1. **CMake 动态库方式**：将主要逻辑编译为动态库，主程序只加载库
2. **CRcpp / Live++**：使用专业的热重载工具（付费）
3. **手动 reload**：使用 `dlopen/dlsym` 加载动态库

### 热重载配置示例

如果需要完整的热重载支持，可以修改 CMake 将模块编译为动态库：

```cmake
# 在 CMakeLists.txt 中
add_library(MeshForgeCore SHARED src/App/Application.cpp ...)
target_link_libraries(MeshForgeApp PRIVATE MeshForgeCore)
```

## 调试技巧

### 日志输出

使用 spdlog：

```cpp
#include "Core/Logger.h"
MF_LOG_INFO("Hello, debug!");
MF_LOG_WARN("Warning message");
MF_LOG_ERROR("Error occurred");
```

### 断点调试

1. 在代码行号左侧点击设置断点
2. 按 `F5` 启动调试
3. 使用调试工具栏控制：
   - `F10`：步过
   - `F11`：步入
   - `Shift+F11`：步出

### 变量监视

在 VSCode 的调试面板中：
- 查看局部变量
- 添加监视表达式
- 查看调用堆栈

## Release 构建

```bash
./scripts/build_release.sh
./build/bin/MeshForgeApp
```

## 常见问题

### 编译错误

```bash
# 清理后重新构建
./scripts/clean.sh
./scripts/build_debug.sh
```

### 找不到 OpenCASCADE

确保已安装：
```bash
brew install opencascade
```

### 调试器无法启动

检查 `launch.json` 中的 `program` 路径是否正确。
