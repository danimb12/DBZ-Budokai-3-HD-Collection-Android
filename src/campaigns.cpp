// dbz3 - Campanas de Dragon Universe hechas con mods.
//
// El juego traduce cada "codigo de historia" a un contenedor #LST de data_cmn con una tabla
// propia (sub_82156F30, EU sub_82156EF0; ver docs/11_campanas/CAMPANAS.md): 1-99, 100-106,
// 2000-2399, 5000-6199 (un bloque de 38 escenas por historia), 8192+, 16384+. Un mod con
// "campana.toml" puede llevar cualquier codigo a otro contenedor (uno suyo, anadido a data_cmn,
// o uno original), sin tocar los de los demas:
//
//   [codigos]
//   5500 = 3990      # escena 0 de la historia 5500 -> contenedor 3990
//   5501 = 3991
//
// Los textos de las escenas son imagenes por idioma (#AZT en data_usi/data_eng/...), que el
// mod sustituye o anade como cualquier otra entrada.
//
// Y puede sumar personajes a la rueda de Dragon Universe (la del juego solo deja 11, fijos en
// sub_8217D2D0); cada uno juega la historia que el juego guarde (o la del mod) para su casilla:
//
//   [rueda]
//   personajes = 27      # Freezer (IDs separados por comas)
#include <atomic>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/generated_init.h"
#include "guest_region.h"
#include "mods.h"

namespace dbz3::campaigns {
namespace {

std::once_flag g_once;
std::unordered_map<int32_t, uint32_t> g_codes;   // codigo -> contenedor
std::unordered_map<int32_t, uint32_t> g_story_codes;   // [codigos_historia]: solo en las casillas de [rueda]
std::vector<int> g_wheel_ids;                    // personajes de mas en la rueda de DU
std::unordered_map<uint32_t, int> g_start_form;  // [forma_jugador]: palabra del combate -> forma inicial
std::unordered_map<uint32_t, int> g_rival_suit;  // [traje_rival]: palabra del combate -> traje del rival
std::unordered_map<uint32_t, int> g_map_model;   // [modelo_mapa]: palabra del combate -> modelo en el mapa
std::unordered_map<uint32_t, int> g_map_suit;    // [traje_mapa]: palabra del combate -> traje en el mapa
std::unordered_map<uint32_t, int> g_map_anim;    // [animacion_mapa]: palabra -> casilla cuyo vuelo del mapa se usa
std::atomic<uint32_t> g_last_battle{0xFFFFFFFFu}; // ultimo combate lanzado (palabra)

void Load() {
  for (const auto& mod : dbz3::ListMods()) {
    if (!mod.enabled) continue;
    std::ifstream f(dbz3::ModsRoot() / mod.name / "campana.toml");
    if (!f) continue;
    std::string line;
    bool in_codes = false, in_story = false, in_wheel = false, in_forms = false, in_suits = false, in_models = false, in_map_suits = false, in_map_anims = false;
    int n = 0;
    while (std::getline(f, line)) {
      const auto hash = line.find('#');
      if (hash != std::string::npos) line.resize(hash);
      if (line.find('[') != std::string::npos) {
        in_codes = line.find("[codigos]") != std::string::npos;
        in_story = line.find("[codigos_historia]") != std::string::npos;
        in_wheel = line.find("[rueda]") != std::string::npos;
        in_forms = line.find("[forma_jugador]") != std::string::npos;
        in_suits = line.find("[traje_rival]") != std::string::npos;
        in_models = line.find("[modelo_mapa]") != std::string::npos;
        in_map_suits = line.find("[traje_mapa]") != std::string::npos;
        in_map_anims = line.find("[animacion_mapa]") != std::string::npos;
        continue;
      }
      const auto eq = line.find('=');
      if (in_wheel && eq != std::string::npos && line.find("personajes") < eq) {
        for (const char* p = line.c_str() + eq + 1; *p;) {
          char* end = nullptr;
          const long id = std::strtol(p, &end, 10);
          if (end == p) {
            ++p;
            continue;
          }
          if (id >= 0 && id < 64) g_wheel_ids.push_back(int(id));
          p = end;
        }
        continue;
      }
      if ((in_forms || in_suits || in_models || in_map_suits || in_map_anims) && eq != std::string::npos) {   // palabra = forma / traje / modelo
        const long v = std::strtol(line.c_str() + eq + 1, nullptr, 10);
        if (v < -1 || v > 255) {   // van a un byte del juego: fuera de rango seria otro personaje o basura
          REXLOG_WARN("dbz3 campanas: '{}': valor fuera de rango ignorado: {}", mod.name, line);
          continue;
        }
        (in_forms ? g_start_form : in_suits ? g_rival_suit : in_models ? g_map_model : in_map_suits ? g_map_suit : g_map_anim)[uint32_t(std::strtoul(line.c_str(), nullptr, 10))] = int(v);
        continue;
      }
      if ((!in_codes && !in_story) || eq == std::string::npos) continue;
      char* end = nullptr;
      const long code = std::strtol(line.c_str(), &end, 10);
      const long cont = std::strtol(line.c_str() + eq + 1, &end, 10);
      if (code <= 0 || cont <= 0) continue;
      (in_story ? g_story_codes : g_codes)[int32_t(code)] = uint32_t(cont);
      ++n;
    }
    REXLOG_INFO("dbz3 campanas: '{}' cambia {} escenas de Dragon Universe", mod.name, n);
  }
}

}  // namespace
}  // namespace dbz3::campaigns

namespace dbz3::campaigns {
std::chrono::steady_clock::time_point g_map_since;
// Forma con la que empieza tu personaje el combate de historia que se acaba de lanzar
// ([forma_jugador] del campana.toml del mod), o -1.
int PlayerStartForm() {
  const auto it = g_start_form.find(g_last_battle.load());
  return it == g_start_form.end() ? -1 : it->second;
}
// Traje del rival en ese combate ([traje_rival]: 0 el normal), o -1.
int RivalSuit() {
  const auto it = g_rival_suit.find(g_last_battle.load());
  return it == g_rival_suit.end() ? -1 : it->second;
}
bool IsModBattle(uint8_t* base, uint32_t word) {
  const uint32_t code = word >> 16;
  if (word == 0xFFFFFFFF || code < 5400 || code >= 5800) return false;
  for (int id : g_wheel_ids) {
    if (REX_LOAD_U16(0x82020668 + uint32_t(id) * 2) == (code - 5000) % 100) return true;
  }
  return false;
}
}  // namespace dbz3::campaigns

DBZ3_HOOK(sub_82156F30, dbz3eu_sub_82156EF0) {
  std::call_once(dbz3::campaigns::g_once, dbz3::campaigns::Load);
  const int32_t code = ctx.r3.s32;
  const uint32_t lr = uint32_t(ctx.lr);
  const auto it = dbz3::campaigns::g_codes.find(code);
  uint32_t cont = it == dbz3::campaigns::g_codes.end() ? 0 : it->second;
  // escenas compartidas del juego (eventos del mapa) con la copia del mod solo mientras se juega
  // la historia de uno de sus personajes de [rueda] (la casilla de DU en 0x824A58A0)
  if (!cont && !dbz3::g_guest_eu) {
    const auto st = dbz3::campaigns::g_story_codes.find(code);
    if (st != dbz3::campaigns::g_story_codes.end()) {
      const uint32_t slot = REX_LOAD_U8(0x824A58A0);
      for (int id : dbz3::campaigns::g_wheel_ids) {
        if (REX_LOAD_U16(0x82020668 + uint32_t(id) * 2) == slot) cont = st->second;
      }
    }
  }
  if (!cont) {
    orig(ctx, base);
  } else {
    ctx.r3.u64 = cont;
  }
  // una linea por escena: quien hace campañas ve que codigo pide cada momento de la historia
  REXLOG_INFO("dbz3 campanas: codigo {} -> contenedor {}{} (desde {:08X})", code, ctx.r3.s32,
              cont ? " (del mod)" : "", lr);
}

// Rueda de Dragon Universe: r3 = objeto de la rueda (+32 = casillas que permite el modo, r4 = las
// desbloqueadas). La de DU es siempre 0x4_00023C9D; los personajes del mod se suman a las dos.
DBZ3_HOOK(sub_8217AD68, dbz3eu_sub_8217AD20) {
  std::call_once(dbz3::campaigns::g_once, dbz3::campaigns::Load);
  constexpr uint64_t kDuMask = 0x400023C9Dull;
  const uint32_t obj = ctx.r3.u32;
  if (obj && !dbz3::campaigns::g_wheel_ids.empty() && REX_LOAD_U64(obj + 32) == kDuMask) {
    uint64_t extra = 0;
    for (int id : dbz3::campaigns::g_wheel_ids) {
      const uint32_t slot = REX_LOAD_U16(dbz3::GuestAddr(0x82020668, 0x82020670) + uint32_t(id) * 2);
      if (slot < 38) extra |= 1ull << slot;
    }
    REX_STORE_U64(obj + 32, kDuMask | extra);
    ctx.r4.u64 |= extra;
    REXLOG_INFO("dbz3 campanas: rueda de Dragon Universe con casillas de mas {:016X}", extra);
  }
  orig(ctx, base);
}

// Escena que arranca: r3 = codigo << 16 | entrada del contenedor (la entrada no llega al registro).
DBZ3_HOOK(sub_8215E918, dbz3eu_sub_8215E8D8) {
  REXLOG_INFO("dbz3 campanas: escena {} entrada {}", ctx.r3.u32 >> 16, ctx.r3.u32 & 0xFFFF);
  orig(ctx, base);
}

// Quien pide la escena siguiente (para seguir una historia en el log): un evento con sus
// parametros (r4+44 -> {entrada, codigo - 8192}) o la vuelta al mapa (codigo 3000, r3 = entrada).
DBZ3_HOOK(sub_8215EC68, dbz3eu_sub_8215EC28) {
  if (const uint32_t p = ctx.r4.u32 ? REX_LOAD_U32(ctx.r4.u32 + 44) : 0) {
    REXLOG_INFO("dbz3 campanas: un evento pide la escena {} entrada {} (desde {:08X})",
                int32_t(REX_LOAD_U32(p + 4)) + 8192, REX_LOAD_U32(p), uint32_t(ctx.lr));
  }
  orig(ctx, base);
}
DBZ3_HOOK(sub_82156B38, dbz3eu_sub_82156AF8) {
  REXLOG_INFO("dbz3 campanas: vuelta al mapa, entrada {} (desde {:08X})", ctx.r3.s32, uint32_t(ctx.lr));
  orig(ctx, base);
}

// Arranque de una escena desde el mapa o la historia (r3 = codigo << 16 | entrada).
DBZ3_HOOK(sub_8215E9C8, dbz3eu_sub_8215E988) {
  dbz3::campaigns::g_last_battle = ctx.r3.u32;
  REXLOG_INFO("dbz3 campanas: se lanza la escena {} entrada {} (desde {:08X})", ctx.r3.u32 >> 16, ctx.r3.u32 & 0xFFFF,
              uint32_t(ctx.lr));
  orig(ctx, base);
}

namespace dbz3::campaigns {
// Depuracion (DBZ3_DU_TRACE=1): cambios de las palabras de "escena pendiente" y de la siguiente
// escena guardada del personaje de DU, una vez por cuadro.
void Trace(uint8_t* base) {
  static const bool on = std::getenv("DBZ3_DU_TRACE") != nullptr;
  if (!on || dbz3::g_guest_eu) return;
  static uint32_t last[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
  const uint32_t slot = REX_LOAD_U8(0x824A58A0);
  const uint32_t blk = 0x8247A9B0 + 128 + 4624 * (slot < 38 ? slot : 0);
  // mapa (0x82479A60): +484 objeto cercano, +3568 su marca (lo que X lanza)
  const uint32_t now[9] = {REX_LOAD_U32(0x8234DD1C), REX_LOAD_U32(0x8234DD24), REX_LOAD_U32(0x8234DD28),
                           REX_LOAD_U32(blk + 4564), REX_LOAD_U32(blk + 4568), REX_LOAD_U32(0x82479A60 + 484),
                           REX_LOAD_U32(0x82479A60 + 3568), REX_LOAD_U32(0x82479A60 + 216),
                           REX_LOAD_U32(0x82479CC8 + 1388)};
  if (std::equal(now, now + 9, last)) return;
  std::copy(now, now + 9, last);
  auto w = [](uint32_t v) { return v == 0xFFFFFFFF ? std::string("-") : std::to_string(v >> 16) + "." + std::to_string(v & 0xFFFF); };
  REXLOG_INFO("dbz3 campanas: traza casilla {} pend {} / {} / {} guardada {} / {} cerca {:08X} marca {:08X} mapa {} combate {}",
              slot, w(now[0]), w(now[1]), w(now[2]), w(now[3]), w(now[4]), now[5], now[6], now[7], int32_t(now[8]));
}
}  // namespace dbz3::campaigns

// Evento del mapa que pide la siguiente escena (r4+44 -> {codigo << 16 | entrada, ...}).
DBZ3_HOOK(sub_82159C18, dbz3eu_sub_82159BD8) {
  if (const uint32_t p = ctx.r4.u32 ? REX_LOAD_U32(ctx.r4.u32 + 44) : 0) {
    const uint32_t v = REX_LOAD_U32(p);
    REXLOG_INFO("dbz3 campanas: el mapa pide la escena {} entrada {} (objeto {:08X}, {:08X} {:08X})", v >> 16, v & 0xFFFF,
                ctx.r4.u32, REX_LOAD_U32(p + 4), REX_LOAD_U32(p + 8));
  }
  orig(ctx, base);
}


DBZ3_EXTERN_EU(dbz3eu_sub_8215E988);

// Combates de las historias de mod: el juego solo los arranca desde el mapa (lanzados nada mas
// acabar una narracion, el combate cuelga en sub_8209F568). IsModBattle: la palabra es un combate
// (bloques 5400-5799) de un personaje de [rueda].

// Seguro de las historias de mod: sub_820AAD30(lista, recurso) libera un recurso de una escena y
// lee recurso+128 sin comprobarlo. Con las escenas de un mod llego a recibir 0x20 (un recurso ya
// liberado) y el juego se cerraba al cargar el dialogo del combate siguiente: un puntero imposible
// se ignora (devuelve 1, "no estaba cargando", como cuando el estado no es 1).
REX_HOOK_RAW(sub_820AAD30) {
  if (ctx.r4.u32 < 0x10000) {
    static int warned = 0;
    if (warned++ < 5)
      REXLOG_WARN("dbz3 campanas: recurso imposible {:08X} al liberar (desde {:08X}): se ignora", ctx.r4.u32,
                  uint32_t(ctx.lr));
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_820AAD30(ctx, base);
}

// Estado "volar por el mapa" (sub_821C4DA0, r3 = tarea del mapa; en +28 su siguiente estado). Las
// historias de mod dejan su combate como marca del mapa (op 24); tras 5 s de mapa (lo que tarda el mapa en quedar libre tras un combate) se toca sola,
// haciendo lo mismo que el juego al tocar una marca de combate (0x821C4B90 / 0x821C5040): marca
// tocada en +232, escena lanzada, mapa al estado 7 (entrar en un evento) y la tarea a su manejador.
// Solo US (las direcciones del mapa estan medidas en la US).
REX_HOOK_RAW(sub_821C4DA0) {
  using namespace dbz3::campaigns;
  // Desde 2026-10-11 las marcas de las historias de mod son como las del juego (icono 1, letrero y
  // punto en el minimapa) y se tocan volando hasta ellas; el toque solo queda para pruebas.
  // DBZ3_AUTO_TOUCH=N: las N primeras marcas (sin numero, todas)
  static int auto_touch = std::getenv("DBZ3_AUTO_TOUCH") ? (std::atoi(std::getenv("DBZ3_AUTO_TOUCH")) > 0 ? std::atoi(std::getenv("DBZ3_AUTO_TOUCH")) : 1 << 30) : 0;
  if (auto_touch > 0 && !dbz3::g_guest_eu && !g_wheel_ids.empty() && REX_LOAD_U32(0x8234DD1C) == 0xFFFFFFFF) {
    const uint32_t slot = REX_LOAD_U8(0x824A58A0);
    const uint32_t marks = 0x8247A9B0 + 128 + 4624 * (slot < 38 ? slot : 0) + 128;
    uint32_t mark = 0;
    for (uint32_t i = 0; i < 64 && !mark; ++i) {
      if (IsModBattle(base, REX_LOAD_U32(marks + 64 * i))) mark = marks + 64 * i;
    }
    if (!mark) {
      g_map_since = {};
    } else if (g_map_since == std::chrono::steady_clock::time_point{}) {
      g_map_since = std::chrono::steady_clock::now();
    } else if (std::chrono::steady_clock::now() - g_map_since > std::chrono::seconds(5)) {
      g_map_since = {};
      --auto_touch;
      const uint32_t task = ctx.r3.u32, word = REX_LOAD_U32(mark);
      REXLOG_INFO("dbz3 campanas: se toca la marca de combate {} entrada {}", word >> 16, word & 0xFFFF);
      constexpr uint32_t kMap = 0x82479A60, kScr = 0x824BA110;
      auto f32 = [&](uint32_t a) { const uint32_t v = REX_LOAD_U32(a); float f; std::memcpy(&f, &v, 4); return f; };
      auto put = [&](uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); REX_STORE_U32(a, v); };
      const float f31 = f32(0x8201F60C);
      const PPCContext saved = ctx;
      REX_STORE_U32(kMap + 232, mark);
      ctx.r3.u64 = mark + 16;                       // camara hacia la marca
      sub_821C3218(ctx, base);
      put(kMap + 200, f31);
      REX_STORE_U32(kMap + 192, 0x10007);
      put(kMap + 204, f32(0x82000D74));
      const uint32_t cam = REX_LOAD_U32(REX_LOAD_U32(REX_LOAD_U32(kMap + 188) + 1132) + 20);
      put(kMap + 196, float(int32_t(REX_LOAD_U32(cam + 120))));
      ctx.r3.u64 = 1;                               // fundido
      ctx.r4.u64 = 0;
      ctx.f1.f64 = f32(kScr + 0x2C0C0);
      sub_82092570(ctx, base);
      if (f32(kScr + 0x2C0C0) > f31) {
        ctx.r3.u64 = 1; ctx.r4.u64 = 0; ctx.r5.u64 = 9; ctx.r6.u64 = 0;
        sub_820922A0(ctx, base);
      }
      ctx.r3.u64 = word;
      sub_8215E9C8(ctx, base);
      ctx.r3.u64 = 10; ctx.r4.u64 = 0x821C7058; ctx.r5.u64 = 0;   // sonido de entrar
      sub_82165190(ctx, base);
      ctx = saved;
      REX_STORE_U32(kMap + 216, 7);
      REX_STORE_U32(task + 28, 0x821C3960);
      return;
    }
  }
  __imp__sub_821C4DA0(ctx, base);
}


namespace dbz3::campaigns {
namespace {
// Aspecto del personaje del mapa ([modelo_mapa] / [traje_mapa] / [animacion_mapa] del combate que
// toca; -1 = el suyo). El combate que toca: la escena guardada del personaje, el ultimo combate
// lanzado y las marcas del mapa; si ninguna palabra esta tal cual, vale otra del mismo codigo (saga).
struct MapLook {
  int model = -1, suit = -1, anim = -1;
  bool operator==(const MapLook& o) const { return model == o.model && suit == o.suit && anim == o.anim; }
};
MapLook PickLook(uint8_t* base, const char* how, bool quiet = false) {
  MapLook l;
  // EU: la casilla de DU (0x824A58A0) no esta localizada en la europea: sin cambio de aspecto
  if (dbz3::g_guest_eu || (g_map_model.empty() && g_map_suit.empty() && g_map_anim.empty())) return l;
  const uint32_t slot = REX_LOAD_U8(0x824A58A0);
  const uint32_t blk = 0x8247A9B0 + 128 + 4624 * (slot < 38 ? slot : 0);
  std::vector<uint32_t> words = {REX_LOAD_U32(blk + 4564), REX_LOAD_U32(blk + 4568), g_last_battle.load()};
  for (uint32_t i = 0; i < 64; ++i) words.push_back(REX_LOAD_U32(blk + 128 + 64 * i));
  auto pick = [&](const std::unordered_map<uint32_t, int>& m) {
    for (uint32_t w : words) {
      if (const auto it = m.find(w); it != m.end()) return it->second;
    }
    for (uint32_t w : words) {
      for (const auto& [k, v] : m) {
        if (w != 0xFFFFFFFFu && (k >> 16) == (w >> 16)) return v;
      }
    }
    return -1;
  };
  l = {pick(g_map_model), pick(g_map_suit), pick(g_map_anim)};
  if (!quiet) REXLOG_INFO("dbz3 campanas: personaje del mapa {} (casilla {}, guardada {:08X}, ultimo {:08X}): modelo {} traje {} vuelo {}",
              how, slot, words[0], words[2], l.model, l.suit, l.anim);
  return l;
}

// Crea al personaje (la llamada del juego `create`) con ese aspecto y lo deja todo como estaba, asi la
// partida guardada no cambia. Modelo y traje en el registro (+4578 / +4579). [animacion_mapa]: las
// entradas de la casilla del personaje en las tablas del mapa, cambiadas solo durante la llamada por
// las de la casilla elegida: vuelo (0x823742C8 + (casilla*5 + k)*4, k = traje si el modelo < 3, si no
// 4) y animaciones (0x823745C0[casilla]). La casilla (+4577) no se cambia: con ella cambiaban tambien
// los datos por casilla y modelo (sub_821C0BE0) y R1 cerraba el juego (2026-10-12).
template <class F>
void WithLook(uint8_t* base, uint32_t rec, const MapLook& l, F&& create) {
  constexpr uint32_t kFlight = 0x823742C8;
  if (!rec) {
    create();
    return;
  }
  const uint8_t old_model = REX_LOAD_U8(rec + 4578), old_suit = REX_LOAD_U8(rec + 4579);
  if (l.model >= 0) REX_STORE_U8(rec + 4578, uint8_t(l.model));
  if (l.suit >= 0) REX_STORE_U8(rec + 4579, uint8_t(l.suit));
  // y el fichero de animaciones del mapa de la casilla (0x823745C0[casilla], map+24): con solo el vuelo
  // Mecha seguia sentado como en la nave (visto por el usuario 2026-10-12)
  constexpr uint32_t kAnims = 0x823745C0;
  uint32_t entry = 0, old_entry = 0, anims = 0, old_anims = 0;
  if (l.anim >= 0 && l.anim < 38) {
    const uint32_t slot = REX_LOAD_U8(rec + 4577);
    const uint32_t k = REX_LOAD_U8(rec + 4578) < 3 ? REX_LOAD_U8(rec + 4579) : 4;
    if (slot < 38 && k < 5) {
      entry = kFlight + (slot * 5 + k) * 4;
      old_entry = REX_LOAD_U32(entry);
      REX_STORE_U32(entry, REX_LOAD_U32(kFlight + (uint32_t(l.anim) * 5 + k) * 4));
      anims = kAnims + slot * 4;
      old_anims = REX_LOAD_U32(anims);
      REX_STORE_U32(anims, REX_LOAD_U32(kAnims + uint32_t(l.anim) * 4));
    }
  }
  create();
  if (entry) REX_STORE_U32(entry, old_entry);
  if (anims) REX_STORE_U32(anims, old_anims);
  REX_STORE_U8(rec + 4578, old_model);
  REX_STORE_U8(rec + 4579, old_suit);
}
}  // namespace
}  // namespace dbz3::campaigns

// Personaje del mapa al volver de un combate o al empezar la historia (sub_821C24B8): lo crea
// sub_82133B88(ID, modelo +4578, traje +4579) con el registro del jugador de DU (0x82479A60 +180).
// Freezer: modelo 5 = Mecha; traje 2 con modelo 0 = el modelo 193, Freezer en su nave. Solo US.
REX_HOOK_RAW(sub_821C24B8) {
  using namespace dbz3::campaigns;
  const uint32_t rec = dbz3::g_guest_eu ? 0 : REX_LOAD_U32(0x82479A60 + 180);
  const MapLook l = rec ? PickLook(base, "al crear") : MapLook{};
  WithLook(base, rec, l, [&] { __imp__sub_821C24B8(ctx, base); });
}

// Al llegar a otro planeta el juego no rehace al personaje del mapa: en la Tierra sigue la nave de
// Namek hasta volver del primer combate. Rehacerlo a mano al llegar (sub_821C24B8 desde el estado de
// volar) cerro el juego (2026-10-12, lectura de 0x14): no se hace.


// sub_820A5820(objeto, recurso, hueco): suelta el recurso (sub_820AAD30) y luego lee recurso+32.
// Tras un relevo del Tag Team, un Kamehameha del que entra le paso 0x20 (2026-10-12: el seguro de
// arriba lo ignoraba pero la lectura siguiente cerraba el juego). Un recurso imposible se trata como
// "sin recurso" (r4 = 0), un caso que la funcion ya contempla.
REX_HOOK_RAW(sub_820A5820) {
  if (ctx.r4.u32 && ctx.r4.u32 < 0x10000) {
    static int warned = 0;
    if (warned++ < 5)
      REXLOG_WARN("dbz3: recurso imposible {:08X} en sub_820A5820 (desde {:08X}): sin recurso", ctx.r4.u32,
                  uint32_t(ctx.lr));
    ctx.r4.u64 = 0;
  }
  __imp__sub_820A5820(ctx, base);
}

// sub_82096FF0(?, ?, id): efecto/sonido `id` del banco global ([0x8238CE10], tabla de desplazamientos
// en [0x8238CE14] desde el id [banco+24]); usa el recurso de la entrada (+20). Tras un relevo del Tag
// Team el Kamehameha del que entra pidio una entrada con recurso 0x20 (el banco es del que salio) y el
// juego se cerraba (2026-10-12). Una entrada imposible no se usa: sin ese efecto, sin cierre.
// 2026-10-12 (prueba de estres del Tag Team): un efecto que seguia en pantalla tras el relevo pidio un id
// que no esta en el banco y el juego llamo a una funcion basura del recurso (vtable[1]): parada del
// juego (0x80000003). Se comprueba lo mismo que el juego va a leer (entrada, recurso, su vtable[1]) y,
// si no es posible, se hace lo que la funcion hace sin banco: 32 bytes a 0 en objeto+224.
REX_HOOK_RAW(sub_82096FF0) {
  const uint32_t bank = REX_LOAD_U32(0x8238CE10);
  const int32_t id = ctx.r5.s32;
  if (bank && id != -1 && !dbz3::g_guest_eu) {
    // id fuera de la tabla del banco: el juego lee una entrada basura. A veces le sale un recurso 0 y no
    // pasa nada, pero R1 en el mapa de la Tierra con Freezer pide uno que cae en memoria sin mapear y el
    // juego se cerraba (2026-10-12, lectura de 0x06A459B2): fuera de la tabla, sin efecto
    auto code = [](uint32_t a) { return a >= 0x82000000u && a < 0x83000000u && !(a & 3); };
    const uint32_t offs = REX_LOAD_U32(0x8238CE14);
    const int32_t idx = id - int32_t(REX_LOAD_U32(bank + 24));
    const uint32_t rel = offs >= 0x10000 && idx >= 0 && idx < 4096 ? REX_LOAD_U32(offs + 4 * uint32_t(idx)) : 0xFFFFFFFFu;
    const uint32_t entry = rel < 0x01000000u ? bank + rel : 0;
    const uint32_t res = entry ? REX_LOAD_U32(entry + 20) : 0;
    // el recurso tiene que ser de la clase de los efectos (vtable 8201CAA0, la unica vista en 14 relevos
    // con tecnicas, DBZ3_FX_VTDIAG 2026-10-12): uno liberado por un relevo y reutilizado por otra clase
    // pasaba el control anterior y el juego acababa en sub_822F5460 -> sub_822F8988 (cierre)
    constexpr uint32_t kFxVtable = 0x8201CAA0;
    const bool ok = entry && (!res || (res >= 0x10000 && REX_LOAD_U32(res) == kFxVtable &&
                                       code(REX_LOAD_U32(REX_LOAD_U32(res) + 4))));
    static const bool vtdiag = std::getenv("DBZ3_FX_VTDIAG") != nullptr;
    if (ok && res && vtdiag) {   // DIAGNOSTICO: clases de recurso que usa el juego
      static std::mutex mu;
      static std::set<uint64_t> seen;
      const uint32_t vt = REX_LOAD_U32(res);
      std::lock_guard<std::mutex> lock(mu);
      if (seen.size() < 32 && seen.insert((uint64_t(vt) << 32) | REX_LOAD_U32(vt + 4)).second)
        REXLOG_INFO("dbz3 fxdiag: recurso {:08X} vtable {:08X} [1]={:08X} id {} desde {:08X}", res, vt,
                    REX_LOAD_U32(vt + 4), id, uint32_t(ctx.lr));
    }
    if (!ok) {
      static int warned = 0;
      if (warned++ < 8)
        REXLOG_WARN("dbz3: efecto {} que no esta en el banco {:08X} (entrada {:08X}, recurso {:08X}, desde {:08X}): sin efecto", id,
                    bank, entry, res, uint32_t(ctx.lr));
      std::memset(base + ctx.r3.u32 + 224, 0, 32);
      return;
    }
  }
  __imp__sub_82096FF0(ctx, base);
}
