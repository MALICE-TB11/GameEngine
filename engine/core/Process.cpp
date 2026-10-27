#include "core/Process.h"

#include <windows.h>

#include <vector>

namespace eng {

int runProcess(const std::wstring& commandLine, std::string& output) {
    output.clear();
    // 匿名管道：子进程把输出写进去，我们从另一端读
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    // 读端不能被子进程继承，否则子进程退出后管道不会关闭，ReadFile 永远等不到结束
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0'); // CreateProcessW 可能修改命令行缓冲，所以要可写副本
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        return -1;
    }

    char buffer[4096];
    DWORD bytes = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &bytes, nullptr) && bytes > 0) output.append(buffer, bytes);
    CloseHandle(readEnd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

} // namespace eng
