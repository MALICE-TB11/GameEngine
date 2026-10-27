#pragma once
// 运行外部程序并捕获输出（shader 热重载用它调用 dxc.exe）。

#include <string>

namespace eng {

// 同步运行命令行，stdout 与 stderr 合并到 output。返回进程退出码；启动失败返回 -1。
int runProcess(const std::wstring& commandLine, std::string& output);

} // namespace eng
