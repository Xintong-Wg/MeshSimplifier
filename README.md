# Mesh Simplifier

Mesh Simplifier 是一个 Windows 优先的 STEP / IGES 轻量化工具。后端复用 MeshForge 的 CAD 解析、BRep 三角化、meshoptimizer 简化和 GLB 导出能力；前端改为轻量 Web Three.js 预览与操作界面。

## 项目结构

```txt
backend/native/       C++ 原生命令行转换器
backend/server/       Node 本地 HTTP 服务，供 Web 调用原生转换器
src/                  从 MeshForge 复用的核心 C++ 模块
web/                  React + Three.js 前端工作台
scripts/              Windows 构建与开发脚本
```

## Windows 依赖

- Visual Studio 2022 C++ 工具链
- CMake 3.25+
- Node.js 20+
- vcpkg，并安装 `opencascade`, `tbb`, `glm`, `spdlog`, `nlohmann-json`, `meshoptimizer`, `draco`

可以直接运行引导脚本安装 vcpkg 和依赖：

```powershell
.\scripts\bootstrap-vcpkg.ps1
```

或使用已有 vcpkg，并设置：

```powershell
$env:VCPKG_ROOT="C:\path\to\vcpkg"
```

## 构建后端

```powershell
.\scripts\build-windows.ps1
```

生成：

```txt
build\bin\Release\MeshSimplifierCli.exe
build\bin\Release\MeshSimplifierServer.exe
```

`MeshSimplifierServer.exe` 是后端服务启动器。双击或从 PowerShell 运行后会打开一个控制台窗口并启动本地后端：

```powershell
.\build\bin\Release\MeshSimplifierServer.exe
```

关闭这个控制台窗口即可关闭后端服务。默认端口为 `8877`，可通过环境变量覆盖：

```powershell
$env:MESH_SIMPLIFIER_PORT="8877"
.\build\bin\Release\MeshSimplifierServer.exe
```

## 命令行转换

```powershell
.\build\bin\Release\MeshSimplifierCli.exe `
  --input "D:\models\assembly.step" `
  --output ".\data\outputs\assembly.glb" `
  --metadata ".\data\outputs\assembly.json" `
  --ratio 0.35
```

## 启动 Web 工作台

```powershell
.\scripts\dev-windows.ps1
```

脚本会启动：

- 本地后端服务：http://127.0.0.1:8877
- 前端开发服务：http://127.0.0.1:5174

后端会在独立控制台窗口中运行，关闭该窗口即可停止后端。

前端输入框使用本机 CAD 文件绝对路径，例如：

```txt
D:\models\assembly.step
```

## 设计说明

前端只参考附件中的布局、组件密度、橙色主交互、半透明面板和 3D 视图组织方式；没有复用附件项目的业务内容、文案或流程。当前界面面向模型简化：左侧输入与装配树，中间 Three.js 视图，右侧压缩统计与处理状态。
