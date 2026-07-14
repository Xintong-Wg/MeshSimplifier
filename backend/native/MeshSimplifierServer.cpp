#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

HANDLE g_childProcess = nullptr;
HANDLE g_jobObject = nullptr;
DWORD g_childProcessId = 0;

std::wstring quoteArg(const std::wstring& value) {
    std::wstring quoted = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') quoted += L'\\';
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

std::wstring getEnvVar(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return L"";
    std::wstring value(size, L'\0');
    GetEnvironmentVariableW(name, value.data(), size);
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

bool envFlagEnabled(const wchar_t* name) {
    const std::wstring value = getEnvVar(name);
    return !value.empty() && value != L"0" && value != L"false" && value != L"FALSE";
}

bool waitForLocalPort(const std::wstring& port, DWORD timeoutMs) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;

    bool ready = false;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    const auto portNumber = static_cast<u_short>(std::stoi(port));

    while (GetTickCount64() < deadline) {
        SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketHandle != INVALID_SOCKET) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(portNumber);
            InetPtonW(AF_INET, L"127.0.0.1", &address.sin_addr);

            if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
                ready = true;
            }
            closesocket(socketHandle);
            if (ready) break;
        }
        Sleep(200);
    }

    WSACleanup();
    return ready;
}

bool fileExists(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

std::optional<fs::path> firstExistingFile(const std::vector<fs::path>& candidates) {
    for (const auto& candidate : candidates) {
        if (fileExists(candidate)) return fs::absolute(candidate);
    }
    return std::nullopt;
}

fs::path executableDir() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (length == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

BOOL WINAPI consoleHandler(DWORD eventType) {
    if (eventType == CTRL_CLOSE_EVENT || eventType == CTRL_C_EVENT || eventType == CTRL_BREAK_EVENT ||
        eventType == CTRL_LOGOFF_EVENT || eventType == CTRL_SHUTDOWN_EVENT) {
        if (g_childProcess) {
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, g_childProcessId);
            WaitForSingleObject(g_childProcess, 2500);
            TerminateProcess(g_childProcess, 0);
        }
        if (g_jobObject) CloseHandle(g_jobObject);
        return TRUE;
    }
    return FALSE;
}

std::optional<fs::path> findProjectRoot(const fs::path& exeDir) {
    std::vector<fs::path> roots = {
        fs::current_path(),
        exeDir,
        exeDir.parent_path(),
        exeDir.parent_path().parent_path(),
        exeDir.parent_path().parent_path().parent_path(),
    };

    for (const auto& root : roots) {
        if (fileExists(root / "backend" / "server" / "src" / "server.js")) {
            return fs::absolute(root);
        }
    }
    return std::nullopt;
}

std::optional<fs::path> findNodeExe(const fs::path& root, const fs::path& exeDir) {
    const std::wstring envNode = getEnvVar(L"MESH_SIMPLIFIER_NODE");
    std::vector<fs::path> candidates;
    if (!envNode.empty()) candidates.emplace_back(envNode);
    candidates.push_back(exeDir / "node.exe");
    candidates.push_back(root / "node.exe");
    candidates.push_back(root / "backend" / "server" / "node.exe");

    if (auto localNode = firstExistingFile(candidates)) return localNode;

    return fs::path(L"node.exe");
}

std::optional<fs::path> findCliExe(const fs::path& root, const fs::path& exeDir) {
    const std::wstring envCli = getEnvVar(L"MESH_SIMPLIFIER_CLI");
    std::vector<fs::path> candidates;
    if (!envCli.empty()) candidates.emplace_back(envCli);
    candidates.push_back(exeDir / "MeshSimplifierCli.exe");
    candidates.push_back(root / "build" / "bin" / "Release" / "MeshSimplifierCli.exe");
    candidates.push_back(root / "build" / "bin" / "MeshSimplifierCli.exe");
    return firstExistingFile(candidates);
}

} // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    const fs::path exeDir = executableDir();
    const auto root = findProjectRoot(exeDir);
    if (!root) {
        std::wcerr << L"Mesh Simplifier backend root not found. Run this exe from the project root or build output folder.\n";
        return 1;
    }

    const auto cli = findCliExe(*root, exeDir);
    if (!cli) {
        std::wcerr << L"MeshSimplifierCli.exe not found. Build the native CLI first.\n";
        return 1;
    }

    const auto node = findNodeExe(*root, exeDir);
    if (!node) {
        std::wcerr << L"node.exe not found. Install Node.js or place node.exe beside MeshSimplifierServer.exe.\n";
        return 1;
    }

    const fs::path serverDir = *root / "backend" / "server";
    const fs::path serverScript = serverDir / "src" / "server.js";
    const std::wstring port = getEnvVar(L"MESH_SIMPLIFIER_PORT").empty() ? L"8877" : getEnvVar(L"MESH_SIMPLIFIER_PORT");

    SetEnvironmentVariableW(L"MESH_SIMPLIFIER_CLI", cli->wstring().c_str());
    SetEnvironmentVariableW(L"MESH_SIMPLIFIER_PORT", port.c_str());

    std::wstring commandLine = quoteArg(node->wstring()) + L" " + quoteArg(serverScript.wstring());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = commandLine;

    g_jobObject = CreateJobObjectW(nullptr, nullptr);
    if (g_jobObject) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_jobObject, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    std::wcout << L"Mesh Simplifier backend starting...\n";
    std::wcout << L"Project root: " << root->wstring() << L"\n";
    std::wcout << L"Native CLI:   " << cli->wstring() << L"\n";
    std::wcout << L"Server URL:   http://127.0.0.1:" << port << L"/\n";
    std::wcout << L"Close this console window to stop the backend service.\n\n";

    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NEW_PROCESS_GROUP,
        nullptr,
        serverDir.wstring().c_str(),
        &startup,
        &process
    );

    if (!created) {
        std::wcerr << L"Failed to start backend server. Windows error: " << GetLastError() << L"\n";
        return 1;
    }

    g_childProcess = process.hProcess;
    g_childProcessId = process.dwProcessId;
    if (g_jobObject) AssignProcessToJobObject(g_jobObject, process.hProcess);
    CloseHandle(process.hThread);

    const std::wstring uiUrl = L"http://127.0.0.1:" + port + L"/";
    if (!envFlagEnabled(L"MESH_SIMPLIFIER_NO_BROWSER")) {
        if (!waitForLocalPort(port, 10000)) {
            std::wcerr << L"Backend port did not become ready in time. Open manually after startup: " << uiUrl << L"\n";
        }
        std::wcout << L"Opening UI in default browser: " << uiUrl << L"\n";
        HINSTANCE opened = ShellExecuteW(nullptr, L"open", uiUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(opened) <= 32) {
            std::wcerr << L"Failed to open default browser. Open manually: " << uiUrl << L"\n";
        }
    }

    const DWORD waitResult = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hProcess);
    if (g_jobObject) {
        CloseHandle(g_jobObject);
        g_jobObject = nullptr;
    }
    g_childProcess = nullptr;
    g_childProcessId = 0;
    return static_cast<int>(exitCode);
}

#else

#include <iostream>

int main() {
    std::cerr << "MeshSimplifierServer is currently only supported on Windows.\n";
    return 1;
}

#endif
