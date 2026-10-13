// dbz3 - EU-only core (Android): no-op stand-ins for the US-only features
// (Tag Team, extended select wheel / new characters, campaigns) that the
// single-EU build leaves out but the shared in-game panels still reference.
// Every stub reports "feature not active", so those panels stay idle.
#include <cstdint>
#include <string>
#include <vector>

#include "tagteam.h"

bool dbz3_select_live() { return false; }
bool dbz3_select_to_stage() { return false; }
void dbz3_select_reset_flow() {}
int dbz3_select_preview_max_form(int) { return 0; }
int64_t dbz3_select_browse_ms(int) { return 0; }
int64_t dbz3_select_now_ms() { return 0; }
int dbz3_select_capsule_option(int) { return -1; }

namespace dbz3::tag {
bool InBattle() { return false; }
HudSide Hud(int) { return {}; }
void SetDynamicTeam(int, const std::vector<int>&, bool) {}
void ClearDynamicTeams() {}
std::vector<int> DynamicTeam(int) { return {}; }
void SetMemberCapsules(int, int, const uint16_t*) {}
void PutCapsules(uint8_t*, uint32_t, int, int) {}
std::vector<int> MyLastTeam() { return {}; }
void SetComSide(int, bool) {}
void BeforeBattleSetup(uint8_t*, uint32_t) {}
uint16_t NetReadyBits() { return 0; }
void NetApplied(uint32_t, uint16_t) {}
void OnScreen(int) {}
}  // namespace dbz3::tag

namespace dbz3::roster {
std::string InternalName(uint32_t) { return {}; }
}  // namespace dbz3::roster

namespace dbz3::campaigns {
void Trace(uint8_t*) {}
}  // namespace dbz3::campaigns
