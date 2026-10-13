// dbz3 - Tag Team: miembros elegidos en la rueda del juego por cada lado (modo TAG TEAM,
// ingame/tagteam_panel.cpp). Son los personajes del juego tal cual: al empezar el combate el
// lado entra con su primer miembro (su ID en la seleccion) y el relevo (tagteam.cpp) cambia el
// luchador por el siguiente miembro de verdad.
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include "src/generated_init.h"
#include "guest_region.h"
#include "roster_ext.h"
#include "tagteam.h"
#include "ingame/tagteam_panel.h"
#include "net/netplay.h"

namespace dbz3::campaigns {
int PlayerStartForm();
int RivalSuit();
}

namespace dbz3::tag {
namespace {
std::mutex g_mu;
std::array<std::vector<int>, 2> g_dyn;   // miembros por lado (vacio = sin equipo)
std::vector<int> g_mine;                 // ultimo equipo de 1P (el que se lleva al online)
std::array<std::map<int, std::array<uint16_t, 8>>, 2> g_caps;   // por lado: ID -> capsulas Custom
}  // namespace

void SetMemberCapsules(int side, int id, const uint16_t* caps) {
  if (side < 0 || side > 1) return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (!caps) {
    g_caps[size_t(side)].erase(id);
    return;
  }
  auto& c = g_caps[size_t(side)][id];
  std::copy(caps, caps + 8, c.begin());
}

void PutCapsules(uint8_t* base, uint32_t rec, int side, int id) {
  std::array<uint16_t, 8> c{};
  bool custom = false;
  if (dbz3::net::Session::Get().link() == dbz3::net::Link::kIdle && side >= 0 && side < 2) {
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_caps[size_t(side)].find(id);
    if (it != g_caps[size_t(side)].end()) {
      c = it->second;
      custom = true;
    }
  }
  if (!custom) {   // las suyas por defecto (char96 +80: n y 7 IDs)
    const uint32_t c96 = 0x8234ABB8 + 96 * uint32_t(id);
    for (uint32_t k = 0; k < 8; ++k) c[k] = REX_LOAD_U16(c96 + 80 + 2 * k);
  }
  for (uint32_t k = 0; k < 8; ++k) REX_STORE_U16(rec + 52 + 2 * k, c[k]);
}

void SetDynamicTeam(int side, const std::vector<int>& members, bool remember) {
  if (side < 0 || side > 1) return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_dyn[size_t(side)] = members;
  if (remember && side == 0 && members.size() >= 2) g_mine = members;
}

std::vector<int> MyLastTeam() {
  std::lock_guard<std::mutex> lock(g_mu);
  // pruebas del online (DBZ3_NET_TEAM="0,11"): el equipo que este PC lleva a la partida online si no
  // eligio uno; solo lo lee el arranque online, no la demo ni los combates normales
  if (g_mine.empty()) {
    if (const char* env = std::getenv("DBZ3_NET_TEAM")) {
      std::vector<int> team;
      for (const char* p = env; *p;) {
        team.push_back(std::atoi(p));
        while (*p && *p != ',') ++p;
        if (*p) ++p;
      }
      return team;
    }
  }
  return g_mine;
}

void ClearDynamicTeams() {
  std::lock_guard<std::mutex> lock(g_mu);
  for (auto& d : g_dyn) d.clear();
  for (auto& c : g_caps) c.clear();
}

std::vector<int> DynamicTeam(int side) {
  std::lock_guard<std::mutex> lock(g_mu);
  return side == 0 || side == 1 ? g_dyn[size_t(side)] : std::vector<int>{};
}

// Inicio del combate (sub_820FE668, r3 = seleccion; ID del jugador k en +32 + 80k): cada lado
// con equipo empieza con su primer miembro.
void BeforeBattleSetup(uint8_t* base, uint32_t sel) {
  if (!sel) return;
  // historias de mod: tu personaje empieza transformado (forma minima y maxima de la seleccion,
  // rec+20/+22: el juego solo carga esa forma y empieza en ella)
  if (const int f = dbz3::campaigns::PlayerStartForm(); f > 0) {
    REX_STORE_U16(sel + 16 + 20, uint16_t(f));
    REX_STORE_U16(sel + 16 + 22, uint16_t(f));
    REXLOG_INFO("dbz3 campanas: tu personaje empieza en la forma {}", f);
  }
  if (const int suit = dbz3::campaigns::RivalSuit(); suit >= 0) {   // rec+18 = traje (0 el normal)
    REX_STORE_U16(sel + 16 + 80 + 18, uint16_t(suit));
    REXLOG_INFO("dbz3 campanas: el rival lleva el traje {}", suit);
  }
  // equipos solo en el modo Tag Team o en una partida online: si quedo alguno de antes (online
  // cerrado a medias, salida por otro camino) no se cuela en Practica ni en un combate normal
  if (!dbz3::ingame::TagTeamMode() && dbz3::net::Session::Get().link() == dbz3::net::Link::kIdle) ClearDynamicTeams();
  for (int k = 0; k < 2; ++k) {
    std::vector<int> team = DynamicTeam(k);
    // pruebas: DBZ3_TAG_TEST="0,11" = equipo del jugador 1 en cualquier combate; DBZ3_TAG_TEST2
    // el del jugador 2 (el rival, para probar el relevo por KO)
    if (const char* env = std::getenv(k ? "DBZ3_TAG_TEST2" : "DBZ3_TAG_TEST"); env && team.empty()) {
      for (const char* p = env; *p;) {
        team.push_back(std::atoi(p));
        while (*p && *p != ',') ++p;
        if (*p) ++p;
      }
      SetDynamicTeam(k, team);
    }
    if (team.size() < 2) continue;
    // su ID y sus capsulas, las elegidas para el (Normal / Custom) en el menu del juego (la seleccion
    // trae las del ultimo confirmado en la rueda: sin esto el primero peleaba con las de otro)
    const uint32_t rec = sel + 16 + 80 * uint32_t(k);
    REX_STORE_U16(rec + 16, uint16_t(team[0]));
    PutCapsules(base, rec, k, team[0]);
    // la casilla nueva recordada es la del ultimo miembro elegido (roster_ext la devolvia a la
    // seleccion y el lado empezaba con el ultimo en vez de con el primero, 2026-10-12)
    dbz3::roster::SetPickedCell(int(k), -1, -1);
    REXLOG_INFO("dbz3 tag: lado {} empieza con el id {} ({} miembros)", k + 1, team[0], team.size());
  }
  // forma inicial elegida con LT/RT en la rueda (en Tag Team, el primer miembro; online, la misma en los dos PCs): forma minima = la
  // elegida (empieza en ella), la maxima no baja de ahi; tope = formas del personaje (char96 +7)
  for (uint32_t k = 0; k < 2; ++k) {
    const uint32_t rec = sel + 16 + 80 * k;
    const int id = int(REX_LOAD_U16(rec + 16));
    if (k == 0 && dbz3::campaigns::PlayerStartForm() > 0) continue;
    const int want = dbz3::ingame::StartForm(int(k));
    if (want <= 0 || id < 0 || id >= 105) continue;   // ponytail: IDs del char96 nativo/ampliado (<105)
    const int forms = REX_LOAD_U8(0x8234ABB8u + uint32_t(id) * 96 + 7);
    // como en el juego: cada forma pide su capsula (char372 +212 + 20*forma); sin ella equipada (las
    // del lado: rec+52 = n y 7 IDs) se empieza en la mas alta que si tenga
    const uint32_t c372 = 0x82329CF0u + 372u * uint32_t(id);
    const uint32_t n = std::min<uint32_t>(REX_LOAD_U16(rec + 52), 7);
    auto equipped = [&](uint32_t cap) {
      for (uint32_t i = 0; i < n; ++i) {
        if (REX_LOAD_U16(rec + 54 + 2 * i) == cap) return true;
      }
      return false;
    };
    int f = std::min(want, forms - 1);
    // medido 2026-10-12: Goku forma g pide la capsula g; con las Normales solo lleva el Kaioken (1)
    while (f > 0 && !equipped(REX_LOAD_U16(c372 + 212 + 20 * uint32_t(f)))) --f;
    if (f <= 0) continue;
    REX_STORE_U16(rec + 20, uint16_t(f));
    if (REX_LOAD_U16(rec + 22) < f) REX_STORE_U16(rec + 22, uint16_t(f));
    REXLOG_INFO("dbz3 forma: lado {} (id {}) empieza en la forma {} de {}", k + 1, id, f, forms);
  }
}

}  // namespace dbz3::tag
