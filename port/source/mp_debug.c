/* LAN match diagnostics (marker `mp_debug`, 2026-11-15).
 *
 * Owner's first Switch-host + emulator-join match: a Hellraiser fist hit killed both players, and the
 * joining player seemed immune to the host's shots. Player::OnHit applies damage on the host only
 * (docs/re/subsystems/player.md), players are matched to network updates by room member id
 * (NetSyncMgr::ReadPlayer -> GetPlayerInfoByMemberId) and every machine also sends its own player's
 * state. This logs, on each machine, every player's member id, local/remote and health whenever they
 * change, so the two machines' logs show which side applied a hit and which side overwrote it.
 * Read-only: nothing in the match is changed. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "so_util.h"
#include "util.h"

extern so_module *port_engine_module(void);

static int on = -1;
static void *(*mp_inst)(void);
static int (*mp_enabled)(void *), (*mp_is_host)(void *);
static void *(*get_mgr)(void);
static int (*info_count)(void *);
static void *(*info_at)(void *, unsigned);
static void *(*info_player)(void *);
static int (*info_member)(void *);
static int (*info_local)(void *);
static float (*dmg_health)(void *);
static int (*info_ready)(void *), (*player_team)(void *), (*actor_dead)(void *);
static void *(*round_mgr)(void), *(*round_session)(void *), *(*gp_inst)(int);

void mp_debug_frame(void) {
  if (on < 0) {
    FILE *f = fopen(PORT_ROOT "/mp_debug", "r");
    on = f != NULL;
    if (f) fclose(f);
    if (!on) return;
    so_module *m = port_engine_module();
    mp_inst = (void *)so_symbol(m, "_ZN11Multiplayer11GetInstanceEv");
    mp_enabled = (void *)so_symbol(m, "_ZN11Multiplayer9IsEnabledEv");
    mp_is_host = (void *)so_symbol(m, "_ZN11Multiplayer6IsHostEv");
    get_mgr = (void *)so_symbol(m, "_Z27GetMultiplayerPlayerManagerv");
    info_count = (void *)so_symbol(m, "_ZNK21CNetPlayerInfoManager18GetPlayerInfoCountEv");
    info_at = (void *)so_symbol(m, "_ZN24MultiplayerPlayerManager22GetPlayerInfoFromIndexEj");
    info_player = (void *)so_symbol(m, "_ZN21MultiplayerPlayerInfo9GetPlayerEv");
    info_member = (void *)so_symbol(m, "_ZNK14CNetPlayerInfo11GetMemberIdEv");
    info_local = (void *)so_symbol(m, "_ZNK14CNetPlayerInfo7IsLocalEv");
    dmg_health = (void *)so_symbol(m, "_ZN9DamageMgr9GetHealthEv");
    info_ready = (void *)so_symbol(m, "_ZN21MultiplayerPlayerInfo20GetReadyForNextMatchEv");
    player_team = (void *)so_symbol(m, "_ZNK6Player7GetTeamEv");
    actor_dead = (void *)so_symbol(m, "_ZNK5Actor6IsDeadEv");
    round_mgr = (void *)so_symbol(m, "_ZN8RoundMgr3GetEv");
    round_session = (void *)so_symbol(m, "_ZN8RoundMgr10GetSessionEv");
    gp_inst = (void *)so_symbol(m, "_ZN9SingletonI11GS_GamePlayE11GetInstanceEb");
    if (!mp_inst || !mp_enabled || !mp_is_host || !get_mgr || !info_count || !info_at || !info_player || !info_member ||
        !info_local || !dmg_health) {
      debugPrintf("[mp] mp_debug: engine functions missing, off\n");
      on = 0;
      return;
    }
    debugPrintf("[mp] mp_debug on: logging players' member id and health in LAN matches\n");
  }
  if (!on) return;
  void *mp = mp_inst();
  if (!mp || !mp_enabled(mp)) return;
  void *mgr = get_mgr();
  if (!mgr) return;
  int n = info_count(mgr);
  if (n < 0) n = 0;
  if (n > 8) n = 8;
  {
    /* GS_GamePlay sub-state (+0x174: 0 play, 7 dying camera, 10 MP death screen, ...) and the round session */
    uint8_t *gp = gp_inst ? gp_inst(0) : NULL;
    int sub = gp ? *(int32_t *)(gp + 0x174) : -1;
    void *rm = round_mgr ? round_mgr() : NULL;
    int sess = rm && round_session ? round_session(rm) != NULL : -1;
    static int last_sub = -99, last_sess = -99;
    if (sub != last_sub || sess != last_sess) {
      debugPrintf("[mp] gameplay sub-state %d -> %d, round session %s\n", last_sub, sub, sess > 0 ? "yes" : sess == 0 ? "none" : "?");
      last_sub = sub; last_sess = sess;
    }
    /* RoundSession (RoundSession::Update, host): +0x2 match-over flag, +0x4 "two players ready" latch,
     * +0xc countdown length (s), +0x14 round length (ms), +0x18 countdown seconds done (== +0xc: round on),
     * +0x48 summary/end state. Logged when any of them changes (not the running ms timer +0x1c). */
    uint8_t *se = rm && round_session ? round_session(rm) : NULL;
    if (se) {
      int v[6] = { se[2], se[4], *(int32_t *)(se + 0xc), *(int32_t *)(se + 0x14), *(int32_t *)(se + 0x18), *(int32_t *)(se + 0x48) };
      static int lv[6] = { -99, -99, -99, -99, -99, -99 };
      if (memcmp(v, lv, sizeof(v))) {
        debugPrintf("[mp] round: over %d, two-ready latch %d, countdown %d/%d s, round length %d ms, end state %d\n", v[0], v[1], v[4],
                    v[2], v[3], v[5]);
        memcpy(lv, v, sizeof(v));
      }
    }
  }
  static struct { void *info; int member, local; float hp; int ready, team, dead; } last[8];
  static int last_n = -1, last_host = -1;
  int host = mp_is_host(mp) & 1;
  if (n != last_n || host != last_host) {
    debugPrintf("[mp] %d player(s), this machine is the %s\n", n, host ? "host" : "client");
    last_n = n;
    last_host = host;
  }
  for (int i = 0; i < n; i++) {
    void *info = info_at(mgr, (unsigned)i);
    if (!info) continue;
    uint8_t *pl = info_player(info);
    int member = info_member(info), local = info_local(info) & 1;
    float hp = pl ? dmg_health(pl + 0x360) : -1; /* Actor +0x360: embedded DamageMgr */
    if (info != last[i].info || member != last[i].member || local != last[i].local) {
      debugPrintf("[mp] slot %d: member %d, %s player%s\n", i, member, local ? "LOCAL" : "remote", pl ? "" : " (no Player yet)");
      last[i].info = info; last[i].member = member; last[i].local = local; last[i].hp = -2; last[i].ready = last[i].team = last[i].dead = -9;
    }
    int ready = info_ready ? info_ready(info) & 1 : -1, team = pl && player_team ? player_team(pl) : -1;
    if (ready != last[i].ready || team != last[i].team) {
      debugPrintf("[mp] member %d (%s): ready %d, team %d\n", member, local ? "local" : "remote", ready, team);
      last[i].ready = ready; last[i].team = team;
    }
    int dead = pl && actor_dead ? actor_dead(pl) & 1 : -1;
    if (dead != last[i].dead) {
      debugPrintf("[mp] member %d (%s): actor %s\n", member, local ? "local" : "remote", dead == 1 ? "DEAD (death animation)" : dead == 0 ? "alive" : "?");
      last[i].dead = dead;
    }
    if (hp != last[i].hp) {
      debugPrintf("[mp] member %d (%s) health %.0f -> %.0f%s\n", member, local ? "local" : "remote", last[i].hp, hp,
                  pl && hp <= 0 && last[i].hp > 0 ? "  DIED" : "");
      last[i].hp = hp;
    }
  }
}
