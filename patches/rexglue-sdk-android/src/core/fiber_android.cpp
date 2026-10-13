/**
 * @file        rex/core/fiber_android.cpp
 * @brief       Android/AArch64 backend for rex::thread::Fiber.
 *
 * Bionic does not implement getcontext/makecontext/swapcontext, so fibers use
 * a minimal hand-written context switch that saves the AAPCS64 callee-saved
 * state (x19-x30, d8-d15, sp) on the fiber's own stack.
 */
#include <rex/platform.h>
#if REX_PLATFORM_ANDROID

#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdint>

#if !defined(__aarch64__)
#error "fiber_android.cpp only supports arm64-v8a"
#endif

extern "C" void rex_fiber_switch(void** save_sp, void* load_sp);
extern "C" void rex_fiber_entry_thunk();

// Frame layout (176 bytes, 16-byte aligned):
//   [sp+0]   d8,d9   [sp+16] d10,d11 [sp+32] d12,d13 [sp+48] d14,d15
//   [sp+64]  x19,x20 [sp+80] x21,x22 [sp+96] x23,x24 [sp+112] x25,x26
//   [sp+128] x27,x28 [sp+144] x29,x30 [sp+160] pad
asm(R"(
  .text
  .global rex_fiber_switch
  .type rex_fiber_switch, %function
  .p2align 4
rex_fiber_switch:
  sub sp, sp, #176
  stp d8,  d9,  [sp, #0]
  stp d10, d11, [sp, #16]
  stp d12, d13, [sp, #32]
  stp d14, d15, [sp, #48]
  stp x19, x20, [sp, #64]
  stp x21, x22, [sp, #80]
  stp x23, x24, [sp, #96]
  stp x25, x26, [sp, #112]
  stp x27, x28, [sp, #128]
  stp x29, x30, [sp, #144]
  mov x2, sp
  str x2, [x0]
  mov sp, x1
  ldp d8,  d9,  [sp, #0]
  ldp d10, d11, [sp, #16]
  ldp d12, d13, [sp, #32]
  ldp d14, d15, [sp, #48]
  ldp x19, x20, [sp, #64]
  ldp x21, x22, [sp, #80]
  ldp x23, x24, [sp, #96]
  ldp x25, x26, [sp, #112]
  ldp x27, x28, [sp, #128]
  ldp x29, x30, [sp, #144]
  add sp, sp, #176
  ret
  .size rex_fiber_switch, .-rex_fiber_switch

  .global rex_fiber_entry_thunk
  .type rex_fiber_entry_thunk, %function
  .p2align 4
rex_fiber_entry_thunk:
  mov x29, #0
  mov x30, #0
  b rex_fiber_trampoline_c
  .size rex_fiber_entry_thunk, .-rex_fiber_entry_thunk
)");

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

extern "C" [[noreturn]] void rex_fiber_trampoline_c() {
  Fiber::Trampoline();
  // A fiber entry must never return (there is no uc_link equivalent).
  __builtin_trap();
}

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);
  uintptr_t top = reinterpret_cast<uintptr_t>(f->stack_.data() + f->stack_.size());
  top &= ~uintptr_t(15);
  top -= 176;
  auto* frame = reinterpret_cast<uint64_t*>(top);
  for (int i = 0; i < 22; i++) frame[i] = 0;
  frame[19] = reinterpret_cast<uint64_t>(&rex_fiber_entry_thunk);  // x30 slot
  f->sp_ = reinterpret_cast<void*>(top);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_switch(&from->sp_, target->sp_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_ANDROID
