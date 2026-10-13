// dbz3 - US/NA vs EU/PAL guest image for the roster / select extension.
//
// The dual-region core runs either image. Both have the same code and the same
// .data tables at the same addresses; only some .rdata tables, .bss globals and the
// function addresses move. roster::ApplyAtLaunch detects the image ("_charasel.cpp"
// signature) and sets g_guest_eu; GuestAddr() then picks the EU address.
// Map + evidence: D:\DBZ3HD\work\eu\INFORME.md (map_addrs.py).
//
// Hooks: the codegen exposes every guest function as a weak symbol (US: sub_X,
// EU: dbz3eu_sub_Y, see tools/prefix_eu_codegen.py). DBZ3_HOOK2 overrides both
// with ONE body `impl(ctx, base, original)`. Builds without the EU codegen
// (single US) only hook the US symbol.
#pragma once

#include <cstdint>

#include <rex/hook.h>

namespace dbz3 {
#if defined(DBZ3_EU_VARIANT) && !defined(DBZ3_DUAL_REGION)
inline bool g_guest_eu = true;
#else
inline bool g_guest_eu = false;
#endif
inline uint32_t GuestAddr(uint32_t us, uint32_t eu) { return g_guest_eu ? eu : us; }
}  // namespace dbz3

using Dbz3GuestFn = void (*)(PPCContext&, uint8_t*);

#if defined(DBZ3_DUAL_REGION)
#define DBZ3_HOOK2(us, eu, impl)                         \
  REX_EXTERN(__imp__##eu);                               \
  REX_HOOK_RAW(us) { impl(ctx, base, __imp__##us); }     \
  REX_HOOK_RAW(eu) { impl(ctx, base, __imp__##eu); }
// call a guest function (or its hook) of the active image
#define DBZ3_CALL2(us, eu) (::dbz3::g_guest_eu ? eu : us)
#define DBZ3_EXTERN_EU(eu) REX_EXTERN(eu)
#elif defined(DBZ3_EU_VARIANT)
// EU-only core (Android): generated_eu is prefixed with
// `tools/prefix_eu_codegen.py --keep-image-config`, so only the EU symbols exist.
#define DBZ3_HOOK2(us, eu, impl) REX_HOOK_RAW(eu) { impl(ctx, base, __imp__##eu); }
#define DBZ3_CALL2(us, eu) eu
#define DBZ3_EXTERN_EU(eu) REX_EXTERN(eu)
#else
#define DBZ3_HOOK2(us, eu, impl) REX_HOOK_RAW(us) { impl(ctx, base, __imp__##us); }
#define DBZ3_CALL2(us, eu) us
#define DBZ3_EXTERN_EU(eu)
#endif

// DBZ3_HOOK(sub_X, dbz3eu_sub_Y) { ... orig(ctx, base); ... }  (body follows the macro)
#define DBZ3_HOOK(us, eu)                                                                   \
  static void dbz3_hook_##us(PPCContext& ctx, uint8_t* base, [[maybe_unused]] Dbz3GuestFn orig); \
  DBZ3_HOOK2(us, eu, dbz3_hook_##us)                                                        \
  static void dbz3_hook_##us(PPCContext& ctx, uint8_t* base, [[maybe_unused]] Dbz3GuestFn orig)
