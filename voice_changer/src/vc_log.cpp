#include "vc_log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace {
std::mutex g_mu;
std::wstring g_path;
bool g_console = false;
}  // namespace

void vc_log_set_file(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_path = path;
}
void vc_log_set_console(bool on) { g_console = on; }

void vc_log(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tm);
    if (g_console) std::printf("[%s] %s\n", ts, msg.c_str());
    if (g_path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path.c_str(), L"ab") == 0 && f) {
        std::fprintf(f, "[%s] %s\r\n", ts, msg.c_str());
        std::fclose(f);
    }
}
