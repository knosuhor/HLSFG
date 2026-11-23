// Minimal file logger. Logs to "mfg-provider.log" next to the dll, so it works in any game
// directory without touching the game or OptiScaler logs.
#pragma once

#include <windows.h>

#include <string>

namespace mfg
{
namespace log
{
// Lazily resolves the directory of our own dll (any address inside it works)
// and opens mfg-provider.log there. Safe to call repeatedly and from either
// entry point (OptiPluginInit runs before the NGX surface does).
void InitFromAddress(const void* addressInsideDll);
void Write(const char* level, const char* fmt, ...);
}
} // namespace mfg

#define MFG_LOG_INFO(...) mfg::log::Write("INFO", __VA_ARGS__)
#define MFG_LOG_WARN(...) mfg::log::Write("WARN", __VA_ARGS__)
#define MFG_LOG_ERROR(...) mfg::log::Write("ERROR", __VA_ARGS__)
