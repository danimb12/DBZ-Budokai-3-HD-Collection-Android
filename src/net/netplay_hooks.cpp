// dbz3 - Online: ganchos del guest para el lockstep (ver netplay.h).
//
// sub_820902F8 (EU 820902C0): sondeo de los 4 mandos, una vez por iteracion del bucle principal
//   = un tick. Se arma el tick antes y, si nadie lo consumio, se juega con el mando en reposo.
// sub_8221C178 (EU 8221A940): thunk de XamInputGetState (r3 = usuario, r4 = X_INPUT_STATE de
//   destino, 0x8238B680 + 16*usuario). Usuario 0: se lee el mando local real (original), se
//   intercambia el tick con el rival y se escribe el del anfitrion; usuario 1: el del invitado
//   (siempre "conectado"). Evidencias: docs/08_online/ONLINE.md.
#include <rex/hook.h>

#include "src/generated_init.h"
#include "../guest_region.h"
#include "netplay.h"
#include "../tagteam.h"
#include "../ingame/menu_pilot.h"
#include "../ingame/online_panel.h"
#include "../ingame/tagteam_panel.h"

namespace dbz3::tag {
bool OnPad(PPCContext& ctx, uint8_t* base, uint32_t user, uint16_t buttons, uint8_t lt);   // tagteam.cpp
void Tick(PPCContext& ctx, uint8_t* base);
}

namespace {

constexpr uint32_t kRngUs = 0x82310670;   // estado del LFSR del juego (US; EU sin mapear)

void WritePad(uint8_t* base, uint32_t out, const dbz3::net::Pad& p, uint32_t packet) {
  REX_STORE_U32(out + 0, packet);
  REX_STORE_U16(out + 4, p.buttons);
  REX_STORE_U8(out + 6, p.lt);
  REX_STORE_U8(out + 7, p.rt);
  REX_STORE_U16(out + 8, uint16_t(p.lx));
  REX_STORE_U16(out + 10, uint16_t(p.ly));
  REX_STORE_U16(out + 12, uint16_t(p.rx));
  REX_STORE_U16(out + 14, uint16_t(p.ry));
}

dbz3::net::Pad ReadPad(uint8_t* base, uint32_t at) {
  dbz3::net::Pad p;
  p.buttons = REX_LOAD_U16(at + 4);
  p.lt = REX_LOAD_U8(at + 6);
  p.rt = REX_LOAD_U8(at + 7);
  p.lx = int16_t(REX_LOAD_U16(at + 8));
  p.ly = int16_t(REX_LOAD_U16(at + 10));
  p.rx = int16_t(REX_LOAD_U16(at + 12));
  p.ry = int16_t(REX_LOAD_U16(at + 14));
  return p;
}

uint32_t Rng(uint8_t* base) { return dbz3::g_guest_eu ? 0 : REX_LOAD_U32(kRngUs); }

// Al empezar el online el anfitrion va solo a 1P VS 2P: su mando lleva abajo, abajo y A (el
// cursor del Duelo esta en COM VS COM, la fila de ONLINE; abajo da la vuelta a 1P VS COM). Va por
// el lockstep como cualquier pulsacion, asi que los dos juegos hacen lo mismo. -1 = nada.
int g_autopick = -1;
uint16_t AutoPick(uint8_t* base) {
  if (g_autopick < 0) return 0;
  constexpr uint16_t kDown = 0x0002, kA = 0x1000;
  constexpr struct { int from, to; uint16_t b; } kSteps[] = {{30, 38, kDown}, {58, 66, kDown}, {86, 94, kA}};
  const int f = g_autopick++;
  if (f == 0 && base[dbz3::GuestAddr(0x824BA110, 0x824BA100)] != 14) g_autopick = -1;   // no esta en el Duelo
  if (f > 94) g_autopick = -1;
  for (const auto& st : kSteps) {
    if (f >= st.from && f < st.to) return st.b;
  }
  return 0;
}

}  // namespace

DBZ3_HOOK(sub_820902F8, dbz3eu_sub_820902C0) {
  auto& s = dbz3::net::Session::Get();
  dbz3::tag::Tick(ctx, base);
  if (!s.active()) dbz3::ingame::pilot::Tick(base);
  if (s.TakeStart(base) && s.role() == dbz3::net::Role::kHost) g_autopick = 0;   // empieza el online
  if (s.active()) s.Arm();
  orig(ctx, base);
  if (s.active() && s.armed()) s.RunTick({}, Rng(base));   // nadie leyo al usuario 0 este tick
}

namespace {
// SELECT del mando de ese usuario para el Tag Team (relevo); se oculta al juego si lo usa.
void TagPad(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t out) {
  const uint16_t b = REX_LOAD_U16(out + 4);
  if (dbz3::tag::OnPad(ctx, base, user, b, REX_LOAD_U8(out + 6))) REX_STORE_U16(out + 4, uint16_t(b & ~0x0020));
}
}  // namespace

DBZ3_HOOK(sub_8221C178, dbz3eu_sub_8221A940) {
  auto& s = dbz3::net::Session::Get();
  const uint32_t user = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  if (!s.active() || user > 1 || !out) {
    orig(ctx, base);
    if (user == 1) dbz3::ingame::NotePad2(ctx.r3.u32 == 0);
    if (user <= 1 && out && ctx.r3.u32 == 0) {
      const uint64_t r3 = ctx.r3.u64;
      if (user == 0) {
        dbz3::ingame::OnlineTick(base, REX_LOAD_U16(out + 4));   // X en los menus
        // la palanca izquierda arriba/abajo cuenta como la cruceta (el teclado usa la palanca)
        constexpr uint16_t kUpDown = 0x0003;
        const uint16_t real = REX_LOAD_U16(out + 4);
        const int16_t ly = int16_t(REX_LOAD_U16(out + 10));
        const uint16_t ext = real | (ly < -16000 ? 0x0002 : 0) | (ly > 16000 ? 0x0001 : 0);
        if (dbz3::ingame::pilot::Busy()) {   // TAG TEAM lleva al juego por sus menus
          const uint16_t piloted = dbz3::ingame::pilot::Filter(ext, 0);
          if (dbz3::ingame::pilot::Busy()) {
            REX_STORE_U16(out + 4, piloted);
            REX_STORE_U32(out + 8, 0);    // sin palancas mientras pilota
            REX_STORE_U32(out + 12, 0);
          }
        } else {                            // nuestras filas en los menus del juego
          const uint16_t res = dbz3::ingame::TeamPickFilter(dbz3::ingame::DuelMenuFilter(base, ext), 0);
          if (dbz3::ingame::MenuRowsTracked()) {
            // menu principal y Duelo: la palanca llega al juego ya convertida en cruceta con el mismo
            // umbral que cuenta las filas (un toque suave movia el cursor del juego y no la cuenta:
            // la fila ONLINE salia entre COM VS 2P y COM VS COM)
            REX_STORE_U16(out + 4, res);
            REX_STORE_U32(out + 8, 0);
            REX_STORE_U32(out + 12, 0);
          } else if (res != ext) {
            REX_STORE_U16(out + 4, uint16_t(res & ~(kUpDown & ~real)));
            if ((ext & ~res) & kUpDown) REX_STORE_U16(out + 10, 0);
          }
        }
      } else {                              // jugador 2: piloto y tira del Tag Team (1P VS 2P)
        const uint16_t real = REX_LOAD_U16(out + 4);
        uint16_t res = dbz3::ingame::pilot::User() == 1 ? dbz3::ingame::pilot::Filter(real, 1) : real;
        res = dbz3::ingame::TeamPickFilter(res, 1);
        if (res != real) REX_STORE_U16(out + 4, res);
      }
      dbz3::ingame::FormPickPad(int(user), REX_LOAD_U8(out + 6), REX_LOAD_U8(out + 7));
      TagPad(ctx, base, user, out);
      ctx.r3.u64 = r3;
    }
    return;
  }
  if (user == 0 && s.armed()) {
    orig(ctx, base);                                     // mando local real -> out
    dbz3::net::Pad local = ctx.r3.u32 == 0 ? ReadPad(base, out) : dbz3::net::Pad{};
    local.buttons |= AutoPick(base) | dbz3::tag::NetReadyBits();   // relevo cargado (Tag Team)
    s.RunTick(local, Rng(base));                         // puede esperar al rival
  }
  if (!s.active()) {                                     // se perdio la conexion en este tick
    ctx.r3.u64 = user;
    ctx.r4.u64 = out;
    orig(ctx, base);
    return;
  }
  WritePad(base, out, s.Applied(user), s.tick());
  dbz3::tag::NetApplied(user, s.Applied(user).buttons);   // el juego no ve los bits del relevo
  REX_STORE_U16(out + 4, uint16_t(REX_LOAD_U16(out + 4) & ~0x0C00));
  dbz3::ingame::FormPickPad(int(user), REX_LOAD_U8(out + 6), REX_LOAD_U8(out + 7));   // misma forma inicial
  TagPad(ctx, base, user, out);                          // mismo mando en los dos PCs: mismo relevo
  ctx.r3.u64 = 0;                                        // X_ERROR_SUCCESS: los dos conectados
}
