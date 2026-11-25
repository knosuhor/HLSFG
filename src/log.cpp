#include "log.h"
#include "mfg_state.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace mfg
{
namespace log
{
namespace
{
std::mutex g_logMutex;
std::wstring g_logDir;

SYSTEMTIME Timestamp()
{
    SYSTEMTIME st {};
    GetLocalTime(&st);
    return st;
}
} // namespace

void InitFromAddress(const void* addressInsideDll)
{
    std::scoped_lock lock(g_logMutex);

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       static_cast<LPCWSTR>(addressInsideDll), &module);
    wchar_t path[MAX_PATH] {};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return;

    std::wstring dir(path);
    const auto pos = dir.find_last_of(L'\\');
    if (pos == std::wstring::npos)
        return;
    dir.resize(pos);
    g_logDir = std::move(dir);
}

void Write(const char* level, const char* fmt, ...)
{
    if (!g_settings.fileLog.load(std::memory_order_relaxed))
        return;

    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    std::scoped_lock lock(g_logMutex);
    if (g_logDir.empty())
        return;

    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\hlsfg.log", g_logDir.c_str());
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    auto st = Timestamp();
    char line[1200];
    int len = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] [%s] %s\r\n", st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds, level, text);
    if (len > 0)
    {
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
    }
    CloseHandle(h);
}
} // namespace log
} // namespace mfg
