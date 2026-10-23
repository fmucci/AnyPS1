/* Minimal stand-ins for the headers of the reference emulator (only what gte_core.cpp needs). Written for this project. */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include "types.h"
#include "common/bitutils.h"
#include "gte_types.h"
#define ALIGN_TO_CACHE_LINE
#define NEVER_INLINE __attribute__((noinline))
#define DebugAssert(x) ((void)0)
#define DEV_LOG(...) ((void)0)
namespace CPU {
struct State { GTE::Regs gte_regs; };
extern State g_state;
}
struct Settings { bool gpu_pgxp_enable = false; bool gpu_pgxp_culling = false; bool gpu_pgxp_preserve_proj_fp = false; };
extern Settings g_settings;
#define ERROR_LOG(...) ((void)0)
#define DefaultCaseIsUnreachable() ((void)0)
