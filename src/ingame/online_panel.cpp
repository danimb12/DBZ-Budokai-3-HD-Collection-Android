// dbz3 - Panel ONLINE del modo Duelo (ver online_panel.h y src/net/netplay.h).
#include "online_panel.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>

#include "../guest_region.h"
#include "src/generated_init.h"
#include "../launcher/i18n.h"
#include "../launcher/launcher_state.h"
#include "../launcher/settings.h"
#include "../net/netplay.h"
#include "../launcher/ui_kit.h"
#include "game_style.h"
#include "native_ui.h"
#include "tagteam_panel.h"

#include <rex/logging.h>

// La ultima IP a la que se unio el jugador: la pantalla ONLINE la trae escrita la proxima vez.
REXCVAR_DEFINE_STRING(dbz3_net_last_ip, "", "DBZ3/Online", "Last host IP joined from the ONLINE screen");
// Mods nativos del launcher: se pueden quitar (sin la fila del menu ni su pantalla)
REXCVAR_DEFINE_BOOL(dbz3_tag_team_enabled, true, "DBZ3/Mods", "TAG TEAM entry in the main menu");
REXCVAR_DEFINE_BOOL(dbz3_online_enabled, true, "DBZ3/Mods", "ONLINE entry in the Duel menu");

namespace dbz3 {
bool GameDimmed();
}

namespace dbz3::tag {
bool InBattle();
void OnScreen(int screen);
}
bool dbz3_select_live();   // select_ext.cpp: la rueda de personajes esta en marcha
bool dbz3_select_to_stage();   // los dos confirmaron: escenario (misma pantalla 14)
void dbz3_select_reset_flow();

namespace dbz3::campaigns {
void Trace(uint8_t* base);   // campaigns.cpp
}

namespace dbz3::ingame {
namespace {

namespace T = dbz3::i18n;
using dbz3::net::Link;
using dbz3::net::Session;

std::atomic<bool> g_wanted{false};
std::atomic<bool> g_open{false};
std::atomic<uint16_t> g_pad{0};          // mando 1 leido fuera del pintado (DeferUi)
std::atomic<bool> g_pad_pending{false};
uint16_t g_prev_select = 0;

rex::input::InputSystem* Input() {
  auto* rt = rex::Runtime::instance();
  return rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
}

// Rotulo "(X) TEXTO" abajo a la derecha del area 4:3 del juego, con el marco naranja y el azul de
// los menus de B3; solo se dibuja (no captura el raton ni el mando).
void EntryBadge(ImGuiIO& io, const char* text) {
  const float h = io.DisplaySize.y, w43 = h * 4.0f / 3.0f, x0 = (io.DisplaySize.x - w43) * 0.5f;
  const ImVec2 p0(x0 + w43 * 0.66f, h * 0.70f), p1(x0 + w43 * 0.95f, h * 0.775f);
  auto* dl = ImGui::GetForegroundDrawList();
  const float r = (p1.y - p0.y) * 0.35f;
  dl->AddRectFilledMultiColor(p0, p1, IM_COL32(20, 70, 150, 235), IM_COL32(20, 70, 150, 235),
                              IM_COL32(8, 30, 80, 235), IM_COL32(8, 30, 80, 235));
  dl->AddRect(p0, p1, IM_COL32(250, 140, 20, 255), r, 0, h * 0.005f);
  const float bh = p1.y - p0.y, cy = (p0.y + p1.y) * 0.5f, cx = p0.x + bh * 0.55f, cr = bh * 0.32f;
  dl->AddCircleFilled(ImVec2(cx, cy), cr, IM_COL32(40, 110, 230, 255));
  dl->AddCircle(ImVec2(cx, cy), cr, IM_COL32(255, 255, 255, 255), 0, h * 0.003f);
  ImFont* font = ImGui::GetFont();
  const float fs = bh * 0.5f;
  const ImVec2 xs = font->CalcTextSizeA(fs, FLT_MAX, 0, "X");
  dl->AddText(font, fs, ImVec2(cx - xs.x * 0.5f, cy - xs.y * 0.5f), IM_COL32(255, 255, 255, 255), "X");
  const float ts = bh * 0.55f;
  const ImVec2 sz = font->CalcTextSizeA(ts, FLT_MAX, 0, text);
  const ImVec2 tp(cx + cr + bh * 0.3f, cy - sz.y * 0.5f);
  for (int dx = -2; dx <= 2; dx += 2)
    for (int dy = -2; dy <= 2; dy += 2) dl->AddText(font, ts, ImVec2(tp.x + dx, tp.y + dy), IM_COL32(0, 20, 50, 255), text);
  dl->AddText(font, ts, tp, IM_COL32(120, 230, 255, 255), text);
}

// Fundido de nuestras filas con el del juego (medido 2026-10-11 con capturas cada 0,5 s): al
// confirmar (A/B) la lista sale en ~0,3 s y la pantalla funde a negro en ~1,5 s; al llegar a un
// menu la pantalla sale de negro en ~1,5 s. El tiempo de llegada y el de salida los anota OnlineTick.
std::atomic<int64_t> g_enter_ms{0}, g_leave_ms{0};   // 0 = sin salida pendiente
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
float MenuFade() {
  const int64_t now = NowMs(), leave = g_leave_ms.load();
  float a = std::clamp(float(now - g_enter_ms.load()) / 1500.0f, 0.0f, 1.0f);
  if (leave) a *= std::clamp(1.0f - float(now - leave) / 300.0f, 0.0f, 1.0f);
  return a;
}
// multiplica el alfa de lo dibujado desde el vertice v0 (colores ABGR de ImGui)
void FadeFrom(ImDrawList* dl, int v0, float alpha) {
  for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
    ImU32& c = dl->VtxBuffer[i].col;
    c = (c & 0x00FFFFFF) | (ImU32(float(c >> 24) * alpha) << 24);
  }
}

// Quinta fila "ONLINE" del menu de Duelo. Medidas de una captura a 1600x900 (area 4:3 desde
// x=200): interior del marco x 340..860, filas centradas en y 245/330/410/492, borde de abajo
// en y 538. El marco se alarga una fila (relleno del color del interior y bordes dorados).
void OnlineRow(ImGuiIO& io, bool focus) {
  namespace S = dbz3::ingame::style;
  const S::Area a = S::GameArea(io);
  auto* dl = ImGui::GetForegroundDrawList();
  const float fade = MenuFade();
  if (fade <= 0.001f) return;
  const int v0 = dl->VtxBuffer.Size;
  // un recuadro propio bajo la lista del Duelo, como el de TAG TEAM del menu principal: marco,
  // interior y franja de agua del juego (data_usi 2033)
  const ImVec2 r0 = a.At(430, 562), r1 = a.At(770, 614);
  S::Frame(dl, r0, r1, a.s);
  if (focus) {
    // la franja del juego sigue en COM VS COM: se apaga, la elegida es ONLINE
    dl->AddRectFilled(a.At(340, 452), a.At(860, 533), IM_COL32(10, 25, 45, 150));
    S::Highlight(dl, r0, r1, float(ImGui::GetTime()));
    S::Speech(dl, a.At(195, 650), a.At(950, 860), a.s,
              T::T("Juega contra otra persona por internet.", "Play against another person over the internet."));
  }
  S::MenuText(dl, ImVec2((r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f), 50 * a.s, "ONLINE", focus, false);
  FadeFrom(dl, v0, fade);
}

// Octava "fila" del menu principal: TAG TEAM en un recuadro con el marco del juego bajo la lista
// (medido a 1600x900: interior de la lista x 105..575 del area, ultima fila en y 560, marco hasta
// y ~625; el bocadillo del juego va de y 646 a 866 y su primera linea de texto esta en y ~680).
void TagTeamRow(ImGuiIO& io, bool focus) {
  namespace S = dbz3::ingame::style;
  const S::Area a = S::GameArea(io);
  auto* dl = ImGui::GetForegroundDrawList();
  const float fade = MenuFade();
  if (fade <= 0.001f) return;
  const int v0 = dl->VtxBuffer.Size;
  // entre el adorno de abajo del marco y el bocadillo: sin tapar el texto de ayuda del juego (2026-10-12:
  // en y 640..694 se comia la primera linea de la ayuda de las otras filas)
  const ImVec2 r0 = a.At(160, 606), r1 = a.At(520, 650);
  if (focus) {
    dl->AddRectFilled(a.At(105, 523), a.At(575, 597), IM_COL32(10, 25, 45, 150));   // Dragon Arena ya no es la elegida
    // el bocadillo del juego entero (y 646..866) tapado por el nuestro: no asoma su texto
    S::Speech(dl, a.At(30, 646), a.At(790, 868), a.s,
              T::T("Equipos de 2 o 3 luchadores: SELECT para relevar al compañero.",
                   "Teams of 2 or 3 fighters: SELECT to switch partners."), -1.0f);
  }
  S::Frame(dl, r0, r1, a.s);
  if (focus) S::Highlight(dl, r0, r1, float(ImGui::GetTime()));
  S::MenuText(dl, ImVec2((r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f), 40 * a.s, "Tag Team", focus, false);
  FadeFrom(dl, v0, fade);
}

}  // namespace

// Pantalla de menu del juego: byte 0x824BA110 (EU 0x824BA100), medido en el juego el 2026-10-10:
// 5 = menu principal, 14 = menu de Duelo (1P VS COM...). BACK abre la ayuda del juego en todos los
// menus, asi que nuestras entradas van con X, que en esos dos menus no hace nada.
constexpr int kMainMenuScreen = 5, kDuelMenuScreen = 14;
std::atomic<int> g_screen{-1};
std::atomic<bool> g_battle{false};
std::atomic<bool> g_online_focus{false};   // la fila ONLINE (quinta del Duelo) tiene el cursor
std::atomic<bool> g_tag_focus{false};      // la fila TAG TEAM (bajo el menu principal) tiene el cursor
std::atomic<bool> g_pad2{false};           // mando 2 conectado (1P VS 2P activo en el Duelo)
uint16_t g_swallow = 0;                     // botones que el juego no ve hasta que se suelten

std::atomic<int> g_main_row{0};             // fila del menu principal (seguida aqui)
// Fila real del menu principal: byte +150 de su objeto (tarea +48), leido por el propio menu en
// sub_8218C5D8 (fila 0 Dragon Universe, 1 Duelo -> pantalla 14, ... 6 Dragon Arena). Solo US; si
// no llega (EU) se usa la cuenta de arriba.
std::atomic<int> g_real_main_row{-1};
std::atomic<int64_t> g_real_main_at{0};
std::atomic<uint32_t> g_main_obj{0};        // objeto del menu principal (+150 = fila)

int GameScreen() { return g_screen.load(); }
uint32_t MainMenuObj() { return NowMs() - g_real_main_at.load() < 500 ? g_main_obj.load() : 0; }
bool MenuRowsTracked() {
  const int sc = g_screen.load();
  return sc == kMainMenuScreen || (sc == kDuelMenuScreen && !dbz3_select_live() && !dbz3_select_to_stage());
}
void NotePad2(bool connected) { g_pad2 = connected; }
bool Pad2Connected() { return g_pad2.load(); }
int MainMenuRow() {
  if (NowMs() - g_real_main_at.load() < 500 && g_real_main_row.load() >= 0) return g_real_main_row.load();
  return g_main_row.load();
}
void SetMainMenuRow(int row) { g_main_row = row; }

bool OnlinePanelWanted() { return g_wanted.load() || Session::Get().link() != Link::kIdle; }
bool OnlinePanel::IsOpen() { return g_open.load(); }

// Quinta fila del Duelo: con el cursor del juego en COM VS COM, abajo pasa a ONLINE (el juego no
// lo ve); arriba vuelve, abajo da la vuelta a 1P VS COM y A abre la pantalla ONLINE.
// El cursor del juego vive en un objeto que cambia de sitio en cada visita, asi que se sigue aqui:
// el menu nace en la fila 0, abajo/arriba recorren las 4 filas (1P VS 2P gris tambien) y dan la vuelta, y cada
// pulsacion cuenta una vez (mantener pulsado no repite en este menu, para no perder la cuenta).
std::atomic<bool> g_close_req{false};
// el juego recuerda la fila del Duelo entre visitas (el piloto de TAG TEAM pulsaba A en COM VS 2P
// creyendo estar en 1P VS COM, 2026-10-12): la cuenta no se reinicia al entrar
std::atomic<int> g_duel_row{0};
int DuelMenuRow() { return g_duel_row.load(); }
void SetDuelMenuRow(int row) { g_duel_row = row; }

uint16_t DuelMenuFilter(uint8_t* base, uint16_t real) {
  (void)base;
  constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kUD = 0x0003, kA = 0x1000, kB = 0x2000, kY = 0x8000;
  using Clock = std::chrono::steady_clock;
  static uint16_t prev = 0;
  int row = g_duel_row.load();
  static Clock::time_point ud_at;     // cuando empezo la ultima pulsacion de arriba/abajo
  static bool ud_cap = false;         // arriba/abajo se corta tras el primer paso (sin repeticion)
  const uint16_t pressed = real & ~prev;
  prev = real;
  g_swallow &= real;   // soltado: ya no se oculta
  if (!(real & kUD)) ud_cap = false;
  // un paso por pulsacion: el juego ve arriba/abajo un momento (lo justo para moverse) y no mas
  auto one_step = [&]() {
    ud_at = Clock::now();
    ud_cap = true;
  };
  auto out = [&]() -> uint16_t {
    uint16_t mask = g_swallow;
    if (ud_cap && Clock::now() - ud_at > std::chrono::milliseconds(120)) mask |= kUD;
    return uint16_t(real & ~mask);
  };
  // la rueda de personajes tambien es la pantalla 14: se distingue por su logica (select_ext.cpp)
  const int screen = (g_screen.load() == kDuelMenuScreen && (dbz3_select_live() || dbz3_select_to_stage())) ? -2
                                                                                                  : g_screen.load();
  // menu principal (7 filas, recuerda la ultima): se sigue igual para el piloto de TAG TEAM
  // con la ultima fila (Dragon Arena), abajo pasa a TAG TEAM (el juego no lo ve); arriba vuelve,
  // abajo da la vuelta a la primera fila y A abre la pantalla TAG TEAM
  if (screen == kMainMenuScreen && !TagTeamWanted() && REXCVAR_GET(dbz3_tag_team_enabled)) {
    g_main_row = MainMenuRow();   // la fila real, si el menu la ha dado
    if (!g_tag_focus.load()) {
      if ((pressed & kDown) && g_main_row.load() == 6) {
        g_tag_focus = true;
        g_swallow |= kDown;
      } else if (pressed & kUD) {
        g_main_row = (g_main_row.load() + ((pressed & kDown) ? 1 : 6)) % 7;
        one_step();
      }
    } else if (pressed & kUp) {
      g_tag_focus = false;
      g_swallow |= kUp;
    } else if (pressed & kDown) {
      g_tag_focus = false;            // el juego da la vuelta a la primera fila
      g_main_row = 0;
      one_step();
    } else if (pressed & kA) {
      g_tag_focus = false;
      g_swallow |= kA;
      TagTeamOpen();
    } else if (pressed & kB) {
      g_tag_focus = false;
    } else if (pressed) {
      g_swallow |= pressed;
    }
    if (g_tag_focus.load()) return 0;
    return out();
  }
  if (screen != kMainMenuScreen) g_tag_focus = false;
  const bool active = screen == kDuelMenuScreen && !g_wanted.load() && REXCVAR_GET(dbz3_online_enabled) &&
                      Session::Get().link() == Link::kIdle;
  if (!active) {
    g_online_focus = false;
    if (screen == kDuelMenuScreen && (pressed & kUD)) {
      g_duel_row = (row + ((pressed & kDown) ? 1 : 3)) % 4;
      one_step();
      return out();
    }
    return uint16_t(real & ~g_swallow);
  }
  if (!g_online_focus.load()) {
    if ((pressed & kDown) && row == 3) {
      g_online_focus = true;          // el juego no lo ve: se queda en COM VS COM
      g_swallow |= kDown;
    } else if (pressed & kUD) {
      row = (row + ((pressed & kDown) ? 1 : 3)) % 4;   // 1P VS 2P sin mando 2 sale gris pero se pisa
      one_step();
    }
  } else if (pressed & kUp) {
    g_online_focus = false;           // vuelve a COM VS COM (el juego no lo ve)
    g_swallow |= kUp;
  } else if (pressed & kDown) {
    g_online_focus = false;           // el juego da la vuelta a la primera fila
    row = 0;
    one_step();
  } else if (pressed & kA) {
    g_wanted = true;                  // pantalla ONLINE
    g_swallow |= kA;
  } else if (pressed & (kB | kY)) {
    g_online_focus = false;           // atras: lo hace el juego
  } else if (pressed) {
    g_swallow |= pressed;
  }
  g_duel_row = row;
  if (g_online_focus.load()) return 0;
  return out();
}

void OnlineTick(uint8_t* base, uint16_t p1_buttons) {
  dbz3::campaigns::Trace(base);
  auto& s = Session::Get();
  constexpr uint16_t kX = 0x4000;
  const int screen = base[dbz3::GuestAddr(0x824BA110, 0x824BA100)];
  dbz3::tag::OnScreen(screen);
  if (screen != kDuelMenuScreen) dbz3_select_reset_flow();
  const bool battle = dbz3::tag::InBattle();
  if (screen != g_screen.load()) {          // llegada a otro menu: sale de negro
    g_enter_ms = NowMs();
    g_leave_ms = 0;
  }
  // A/B en el menu principal o en el Duelo sin nuestra fila elegida: el juego sale de ese menu
  if ((screen == kMainMenuScreen && !g_tag_focus.load()) || (screen == kDuelMenuScreen && !g_online_focus.load())) {
    if ((p1_buttons & ~g_prev_select & 0x3000) && !g_leave_ms.load()) g_leave_ms = NowMs();
  }
  if (g_leave_ms.load() && NowMs() - g_leave_ms.load() > 2500) {   // no salio (opcion bloqueada): vuelve
    g_enter_ms = NowMs();
    g_leave_ms = 0;
  }
  g_screen = screen;
  g_battle = battle;
  if ((p1_buttons & kX) && !(g_prev_select & kX) && !battle && s.link() != Link::kPlaying) {
    if (screen == kMainMenuScreen && REXCVAR_GET(dbz3_tag_team_enabled)) {
      TagTeamOpen();
    }
  }
  g_prev_select = p1_buttons;
  // anfitrion: foto del estado de menu (desbloqueos, ultima eleccion) que viaja al empezar
  if (s.role() == dbz3::net::Role::kHost && (s.link() == Link::kConnected || s.link() == Link::kStarting)) {
    s.SetUiSnapshot(base + dbz3::GuestAddr(Session::kUiSnap, Session::kUiSnapEu), Session::kUiSnapSize);
  }
}

OnlinePanel::OnlinePanel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

OnlinePanel::~OnlinePanel() {
  if (blocked_) {
    if (auto* in = Input()) in->RemoveUIInputBlocker();
  }
}

uint16_t OnlinePanel::Pad() {
  if (!g_pad_pending.exchange(true)) {
    dbz3::launcher::DeferUi([]() {
      uint16_t b = 0;
      if (auto* in = Input()) {
        rex::input::X_INPUT_STATE st = {};
        if (in->GetStateForUI(0, &st) == 0) {
          b = static_cast<uint16_t>(st.gamepad.buttons);
          const int lx = int16_t(st.gamepad.thumb_lx), ly = int16_t(st.gamepad.thumb_ly);   // la palanca tambien
          if (ly > 16000) b |= 0x0001;
          if (ly < -16000) b |= 0x0002;
          if (lx < -16000) b |= 0x0004;
          if (lx > 16000) b |= 0x0008;
        }
      }
      g_pad = b;
      g_pad_pending = false;
    });
  }
  const uint16_t b = g_pad.load();
  const uint16_t pressed = b & ~last_buttons_;
  last_buttons_ = b;
  return pressed;
}

void OnlinePanel::OnDraw(ImGuiIO& io) {
  if (dbz3::GameDimmed()) return;   // fuera de la ventana: solo la pantalla oscura del juego
  native_ui::Prefetch();   // piezas del juego para las pantallas propias (una vez, en su hilo)
  auto& s = Session::Get();
  // salir pedido desde la pantalla: lo ejecuta este hilo (dueno del socket salvo en partida); en
  // partida lo cierra el hilo del juego (RunTick), que es el que usa el socket
  if (g_close_req.exchange(false)) {
    if (s.link() == Link::kPlaying) {
      s.RequestLeave();
    } else {
      s.Close();
      REXLOG_INFO("dbz3 online: sesion cerrada por el jugador");
    }
  }
  if (s.link() != Link::kIdle && s.link() != Link::kPlaying) s.Poll();
  const bool show = g_wanted.load() && s.link() != Link::kPlaying;
  auto* in = Input();
  if (show != blocked_ && in) {           // mientras se ve, el juego no recibe los mandos
    if (show) in->AddUIInputBlocker(); else in->RemoveUIInputBlocker();
    blocked_ = show;
  }
  if (show && !g_open.load()) {   // la A que abre la pantalla no cuenta (el mando llega un poco tarde)
    wait_release_ = true;
    opened_at_ = ImGui::GetTime();
  }
  g_open = show;
  if (s.link() == Link::kPlaying) g_wanted = false;
  // conexion perdida o error: la pantalla vuelve sola (fuera del combate) con el motivo
  const Link now = s.link();
  if (int(now) != last_link_ && (now == Link::kLost || now == Link::kError) && !g_battle.load()) g_wanted = true;
  last_link_ = int(now);
  if (!show) {
    if (s.link() == Link::kPlaying && s.desynced()) {   // aviso en la partida (el juego no tiene pantalla para esto)
      ImGui::GetForegroundDrawList()->AddText(ImVec2(16, 16), IM_COL32(255, 70, 70, 255),
                                              T::T("DESINCRONIA: los dos PCs ya no ven lo mismo. Cerrad y volved a empezar.",
                                                   "DESYNC: both PCs no longer see the same thing. Close and start again."));
    }
    editing_ = false;
    // entradas propias en los menus del juego: rotulo con el boton X, como los avisos del juego
    const int screen = g_screen.load();
    // jugando online, en el menu de Duelo: X sale del online (lo lee este PC, no el lockstep)
    if (s.link() == Link::kPlaying && !g_battle.load() && g_screen.load() == kDuelMenuScreen &&
        !dbz3_select_live() && !dbz3_select_to_stage()) {
      EntryBadge(io, T::T("SALIR DEL ONLINE", "LEAVE ONLINE"));
      if (Pad() & 0x4000) g_close_req = true;
    }
    if (!g_battle.load() && !TagTeamWanted() && !TagTeamGate() && s.link() == Link::kIdle) {
      if (screen == kMainMenuScreen && REXCVAR_GET(dbz3_tag_team_enabled)) {
        TagTeamRow(io, g_tag_focus.load());   // US y EU: los menus son iguales
      }
      if (screen == kDuelMenuScreen && REXCVAR_GET(dbz3_online_enabled) && !dbz3_select_live() && !dbz3_select_to_stage()) {
        OnlineRow(io, g_online_focus.load());
      }
    }
    return;
  }
  uint16_t pressed = Pad();
  if (wait_release_) {
    if (g_pad.load() == 0 && ImGui::GetTime() - opened_at_ > 0.3) wait_release_ = false;
    pressed = 0;
  }
  DrawScreen(io, editing_ ? 0 : pressed);   // escribiendo la IP, el teclado no mueve el menu
}

// Pantalla con el aspecto del Duelo: fondo de Kame House, cinta "Online", marco con filas y
// el Kaio del Norte (sacado del juego) que explica cada fila y el estado de la conexion.
void OnlinePanel::DrawScreen(ImGuiIO& io, uint16_t pressed) {
  namespace S = dbz3::ingame::style;
  constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kA = 0x1000, kB = 0x2000,
                     kY = 0x8000;
  auto& s = Session::Get();
  const Link link = s.link();
  const bool idle = link == Link::kIdle || link == Link::kError || link == Link::kLost;
  const bool host = s.role() == dbz3::net::Role::kHost;

  // filas segun el estado
  enum Row { kHostRow, kJoinRow, kIpRow, kPortRow, kDelayRow, kStartRow, kCancelRow };
  std::vector<Row> rows;
  if (idle) {
    rows = {kHostRow, kJoinRow, kIpRow, kPortRow, kDelayRow};
  } else {
    if (host) rows.push_back(kStartRow);
    rows.push_back(kCancelRow);
  }
  if (!ip_[0] && !ip_loaded_) {   // la de la ultima vez
    std::snprintf(ip_, sizeof(ip_), "%s", REXCVAR_GET(dbz3_net_last_ip).c_str());
    ip_loaded_ = true;
  }
  const int n = int(rows.size());
  row_ = std::clamp(row_, 0, n - 1);
  if (pressed & kUp) row_ = (row_ + n - 1) % n;
  if (pressed & kDown) row_ = (row_ + 1) % n;
  const Row cur = rows[size_t(row_)];
  const int step = (pressed & kRight) ? 1 : (pressed & kLeft) ? -1 : 0;
  if (cur == kPortRow && step) port_ = std::clamp(port_ + step, 1024, 65535);
  if (cur == kDelayRow && step) delay_ = std::clamp(delay_ + step, 1, 8);
  const bool can_start = link == Link::kConnected && s.mods_match();
  if (pressed & kA) {
    last_err_.clear();   // una accion nueva borra el error anterior
    switch (cur) {
      case kHostRow:
        s.Host(uint16_t(port_), uint8_t(delay_), &last_err_);
        row_ = 0;
        break;
      case kJoinRow:
        if (ip_[0]) {
          if (s.Join(ip_, uint16_t(port_), &last_err_) && REXCVAR_GET(dbz3_net_last_ip) != ip_) {
            rex::cvar::SetFlagByName("dbz3_net_last_ip", ip_);
            dbz3::settings::SaveUserSettings();
          }
          row_ = 0;
        } else {
          row_ = 2;
          editing_ = true;   // sin IP: a escribirla
        }
        break;
      case kIpRow: editing_ = true; break;
      case kStartRow:
        if (can_start) s.RequestStart();
        break;
      case kCancelRow: g_close_req = true; break;
      default: break;
    }
  }
  if (pressed & (kB | kY)) {
    if (idle) {
      last_err_.clear();
      g_wanted = false;   // cerrar (una sesion perdida vuelve a quedar libre)
      if (link != Link::kIdle) g_close_req = true;
    } else {
      g_close_req = true;   // B con una partida a medias: la cancela
    }
  }

  const S::Area a = S::GameArea(io);
  auto* dl = ImGui::GetBackgroundDrawList();
  const float t = float(ImGui::GetTime());
  // como el Duelo, del que sale: su fondo, el Kaio del Norte donde esta alli y las tiras doradas encima
  S::Backdrop(dl, a, "fondo_duelo");
  S::Host(dl, a, "anfitrion_online", 781, 336, false);
  S::Sides(dl, a);
  S::Banner(dl, a, "Online", "titulo_online");
  constexpr float kStep = 72;
  const ImVec2 f0 = a.At(330, 175), f1 = a.At(870, 175 + kStep * n + 20);
  S::Frame(dl, f0, f1, a.s);
  char buf[96];
  for (int i = 0; i < n; ++i) {
    const ImVec2 r0(f0.x, f0.y + (10 + kStep * i) * a.s), r1(f1.x, r0.y + (kStep - 2) * a.s);
    const ImVec2 c((r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f);
    const bool sel = i == row_;
    if (sel) S::Highlight(dl, r0, r1, t);
    const char* label = "";
    bool disabled = false;
    switch (rows[size_t(i)]) {
      case kHostRow: label = T::T("CREAR PARTIDA", "HOST A MATCH"); break;
      case kJoinRow:
        label = T::T("UNIRSE", "JOIN");
        disabled = !ip_[0];
        break;
      case kIpRow:
        std::snprintf(buf, sizeof(buf), T::T("CODIGO  %s", "CODE  %s"), ip_[0] ? ip_ : "- - -");
        label = buf;
        break;
      case kPortRow:
        std::snprintf(buf, sizeof(buf), T::T("PUERTO  %d", "PORT  %d"), port_);
        label = buf;
        break;
      case kDelayRow:
        std::snprintf(buf, sizeof(buf), T::T("RETARDO  %d", "DELAY  %d"), delay_);
        label = buf;
        break;
      case kStartRow:
        label = T::T("EMPEZAR", "START");
        disabled = !can_start;
        break;
      case kCancelRow: label = T::T("CANCELAR", "CANCEL"); break;
    }
    if (rows[size_t(i)] == kIpRow && editing_) {
      // la IP se escribe con el teclado en la misma fila (Intro o Esc terminan)
      ImGui::SetNextWindowPos(ImVec2(r0.x + 40 * a.s, c.y - 26 * a.s), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(r1.x - r0.x - 80 * a.s, 0), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.0f);
      ImGui::Begin("##dbz3_online_ip", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
      ImFont* f = dbz3::launcher::ui::GetFonts().menu;
      if (f) ImGui::PushFont(f);
      ImGui::SetWindowFontScale(0.7f * a.s);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.02f, 0.10f, 0.25f, 0.85f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.01f, 0.82f, 0.90f, 1.0f));
      ImGui::SetNextItemWidth(-1);
      if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
      ImGui::InputTextWithHint("##ip", "XXXXX-XXXXX", ip_, sizeof(ip_), ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CharsUppercase);
      if (ImGui::IsItemDeactivated()) editing_ = false;
      ImGui::PopStyleColor(2);
      if (f) ImGui::PopFont();
      ImGui::End();
    } else {
      S::MenuText(dl, c, 50 * a.s, label, sel && !disabled, disabled);
    }
    if (sel && (rows[size_t(i)] == kPortRow || rows[size_t(i)] == kDelayRow)) {
      S::Arrows(dl, ImVec2(r0.x + 30 * a.s, r0.y), ImVec2(r1.x - 30 * a.s, r1.y), 40 * a.s, t);
    }
  }

  // el anfitrion explica la fila elegida; con una partida en marcha, el estado
  std::string say;
  if (!last_err_.empty()) say = last_err_ + "\n";
  if (idle) {
    if (link != Link::kIdle) say += s.Status() + "\n";
    switch (cur) {
      case kHostRow:
        say += T::T("Crea la partida: te dara un codigo de sala para pasarselo al otro jugador. Necesitais la misma version y los mismos mods.",
                    "Host the match: you get a room code to give to the other player. You both need the same version and mods.");
        break;
      case kJoinRow:
      case kIpRow:
        say += T::T("Escribe el codigo de sala del anfitrion (pulsa A y usa el teclado; tambien vale su IP) y unete.",
                    "Type the host's room code (press A and use the keyboard; their IP also works), then join.");
        break;
      case kPortRow:
        say += T::T("Puerto UDP. No suele hacer falta tocarlo: el juego intenta abrirlo solo en el router.",
                    "UDP port. Usually no need to change it: the game tries to open it on the router by itself.");
        break;
      default:
        say += T::T("Mas retardo, mas suave con mala conexion: 2-3 en casa, 4-6 por internet.",
                    "More delay is smoother on a bad connection: 2-3 at home, 4-6 online.");
        break;
    }
  } else {
    say += s.Status();
    if (host) {
      // codigo de sala: se copia solo al portapapeles cada vez que cambia (red local -> publica)
      std::string how;
      const std::string code = s.RoomCode(&how);
      if (!code.empty() && code != copied_code_) {
        ImGui::SetClipboardText(code.c_str());
        copied_code_ = code;
      }
      say += T::T("\nCodigo de sala: ", "\nRoom code: ") + code + T::T("  (copiado)", "  (copied)");
      if (how == "router") {
        say += T::T("\nPuerto abierto en tu router: vale por internet.", "\nPort opened on your router: works over the internet.");
      } else if (how == "stun") {
        say += T::T("\nPor internet puede necesitar abrir el puerto UDP en el router.",
                    "\nOver the internet the UDP port may need to be forwarded on the router.");
      } else {
        say += T::T("\nCodigo de tu red de casa (buscando el de internet...).",
                    "\nHome network code (looking for the internet one...).");
      }
    }
    if (link == Link::kConnected || link == Link::kStarting) {
      if (s.mods_match()) {
        say += host ? T::T("\nMods iguales. Pulsa EMPEZAR: los dos ireis a la eleccion de personajes (1P VS 2P).",
                           "\nSame mods. Press START: you both go to character select (1P VS 2P).")
                    : T::T("\nMods iguales. Esperando a que el anfitrion empiece...",
                           "\nSame mods. Waiting for the host to start...");
      } else {
        say += T::T("\nMods distintos: activad los mismos y volved a conectar.",
                    "\nDifferent mods: enable the same ones and reconnect.");
        int shown = 0;
        for (const auto& m : s.ModDiff()) {
          if (shown++ == 2) break;
          say += "\n- " + m;
        }
      }
    }
  }
  S::Speech(dl, a.At(195, 615), a.At(950, 845), a.s, say.c_str(), 0.78f);
  S::HelpLine(dl, a.At(745, 870), 38 * a.s,
              {{"A", T::T("Aceptar", "OK")}, {"B", idle ? T::T("Atras", "Back") : T::T("Cancelar", "Cancel")}});
}

}  // namespace dbz3::ingame

// EU: sub_8218C5A0 (mismo cuerpo, mismos campos +48/+150)
DBZ3_HOOK(sub_8218C5D8, dbz3eu_sub_8218C5A0) {
  const uint32_t obj = REX_LOAD_U32(ctx.r3.u32 + 48);
  if (obj) {
    const int row = REX_LOAD_U8(obj + 150);
    if (dbz3::ingame::g_real_main_row.exchange(row) < 0) REXLOG_INFO("dbz3 menu: fila real del menu principal {}", row);
    dbz3::ingame::g_main_obj = obj;
    dbz3::ingame::g_real_main_at = dbz3::ingame::NowMs();
  }
  orig(ctx, base);
}
