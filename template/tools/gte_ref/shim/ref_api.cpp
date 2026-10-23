#include "shim.h"
CPU::State CPU::g_state;
Settings g_settings;
namespace GTE {
struct { bool freecam_active = false; } s_config;
}
#include "gte_core.cpp"
extern "C" {
void ref_reset() { std::memset(&CPU::g_state.gte_regs, 0, sizeof(GTE::Regs)); }
void ref_write(unsigned n, uint32_t v) { GTE::WriteRegister(n, v); }
uint32_t ref_read(unsigned n) { return GTE::ReadRegister(n); }
void ref_exec(uint32_t w) { GTE::ExecuteInstruction(w); }
uint32_t ref_raw(unsigned n) { return CPU::g_state.gte_regs.r32[n]; }
}
