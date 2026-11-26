#include "backends.h"

#include "lsfg_backend.h"

namespace mfg
{
namespace
{
LsfgBackend g_lsfg;
}

IFGBackend* ActiveBackend()
{
    return static_cast<IFGBackend*>(&g_lsfg);
}
} // namespace mfg
