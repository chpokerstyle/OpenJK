/*
===========================================================================
MagicWands MOBA mod - OpenJK game code (server-side) - implementation
===========================================================================
*/

#include "g_local.h"
#include "g_moba.h"

void SetTeamQuick(gentity_t *ent, int team, qboolean doBegin);

static void MOBA_LoadHeroes( void );
static int MOBA_AbilityPower( gentity_t *ent, const mobaAbility_t *ab );
static void MOBA_BuyItem( gentity_t *ent, int id );

#define MOBA_DEFAULT_MAXHEALTH	500
#define MOBA_DEFAULT_ARMOR		50

// JKA armor is a shield pool that depletes as it eats damage (G_ApplyArmor),
// not a mitigation percentage, so this is a shield point budget. It has to stay
// above the strongest hero base armor (100) or the armor items are dead gold
// for exactly the heroes that are supposed to buy them.
#define MOBA_MAX_ARMOR			200
#define MOBA_MAX_HEALTH			10000

// gold and xp for a hero kill, the round win and round loss payouts are in
// MOBA_StartRoundEnd
#define MOBA_GOLD_KILL			300
#define MOBA_XP_KILL_BASE		50
#define MOBA_XP_KILL_PER_LEVEL	10

#define MOBA_FALLBACK_RESPAWN	3600000	// 1h - effectively never (round-based respawn)

static qboolean mobaEnabled = qfalse;
static mobaPhase_t mobaPhase = MOBA_PHASE_LOBBY;
static int mobaPhaseEnd = 0;
static int mobaRound = 0;
static int mobaRedAlive = 0, mobaBlueAlive = 0;
static int mobaRedPlayers = 0, mobaBluePlayers = 0;

mobaPlayer_t mobaPlayers[MAX_CLIENTS];

//=========================================================================
// Hero / item tables
//=========================================================================

#define AB(n,t,cd,d,pl,r,rad,dur,mul,desc) { n, t, cd, d, pl, r, rad, dur, mul, desc }

mobaHero_t mobaHeroes[MOBA_MAX_HEROES];
int mobaNumHeroes = 0;

mobaItem_t mobaItems[] = {
	{ "Sturdy Armor",	"armor",	"ARMOR",	250,	50,	0,	0,	"+50 armor" },
	{ "Med Kit",		"med",		"HEAL",		200,	0,	100, 0,	"+100 health" },
	{ "Rage Rune",		"rage",		"POWER",	300,	0,	0,	20,	"+20% damage" },
	{ "Heavy Plate",	"heavy",	"ARMOR",	500,	100, 50, 0,	"+100 armor, +50 health" },
	{ "Power Crystal",	"crystal",	"POWER",	650,	0,	50,	40,	"+50 health, +40% damage" },
	{ "Shadow Cloak",	"cloak",	"ARMOR",	400,	30,	0,	15,	"+30 armor, +15% damage" }
};
int mobaNumItems = ARRAY_LEN( mobaItems );

//=========================================================================
static void MOBA_ResetPlayers( void )
{
	int i;

	memset( mobaPlayers, 0, sizeof( mobaPlayers ) );

	// memset leaves heroId at 0, which reads as "hero 0 already picked" and makes
	// the whole draft phase a no-op, so mark every slot as empty explicitly.
	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		mobaPlayers[i].heroId = -1;
	}
}

//=========================================================================
// Phase management
//=========================================================================
// Logs every bit of player-facing mod output into games.log, so a headless
// dedicated server can be verified without a connected client.
//=========================================================================
static void MOBA_LogLine( const char *msg, gentity_t *who )
{
	char	clean[512];
	int		i;

	if ( !moba_log.integer )
	{
		return;
	}

	Q_strncpyz( clean, msg, sizeof( clean ) );
	for ( i = 0; clean[i]; i++ )
	{
		if ( clean[i] == '^' )
		{
			clean[i] = '*';
			if ( clean[i + 1] )
			{
				clean[i + 1] = '*';
			}
		}
	}

	if ( who && who->client )
	{
		G_LogPrintf( "[MOBA] %s: %s\n", who->client->pers.netname, clean );
	}
	else
	{
		G_LogPrintf( "[MOBA] %s\n", clean );
	}
}

static void MOBA_CPAll( const char *fmt, ... )
{
	va_list argptr;
	char msg[512];

	va_start( argptr, fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );

	MOBA_LogLine( msg, NULL );
	trap->SendServerCommand( -1, va( "cp \"%s\n\"", msg ) );
}

static void MOBA_CPSelf( gentity_t *ent, const char *fmt, ... )
{
	va_list argptr;
	char msg[512];

	va_start( argptr, fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );

	MOBA_LogLine( msg, ent );
	trap->SendServerCommand( ent->s.number, va( "cp \"%s\n\"", msg ) );
}

static void MOBA_Self( gentity_t *ent, const char *fmt, ... )
{
	va_list argptr;
	char msg[768];

	va_start( argptr, fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );

	MOBA_LogLine( msg, ent );
	trap->SendServerCommand( ent->s.number, va( "print \"^5[MOBA]^7 %s\n\"", msg ) );
}


//=========================================================================
// Hero assignment
//=========================================================================

static qboolean MOBA_HeroTaken( int heroId )
{
	int i;

	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		if ( mobaPlayers[i].inuse && mobaPlayers[i].heroId == heroId )
		{
			return qtrue;
		}
	}

	return qfalse;
}

// Hands a free hero to every player that has none, so the draft falling through
// on a timeout costs a choice and not the whole round. Must run before the
// respawn of the buy phase, the stats are only applied on spawn.
static void MOBA_FillMissingHeroes( void )
{
	int i, heroId, guard;

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( !mobaPlayers[i].inuse || mobaPlayers[i].heroId >= 0 )
		{
			continue;
		}

		// start random and walk on until a free hero shows up, with fewer
		// clients than heroes the walk always ends before the list runs out
		heroId = Q_irand( 0, mobaNumHeroes - 1 );
		for ( guard = 0; guard < mobaNumHeroes && MOBA_HeroTaken( heroId ); guard++ )
		{
			heroId = ( heroId + 1 ) % mobaNumHeroes;
		}

		mobaPlayers[i].heroId = heroId;

		if ( ent->inuse && ent->client )
		{
			MOBA_LogLine( va( "%s plays %s", ent->client->pers.netname,
				mobaHeroes[heroId].name ), ent );
		}
	}
}

static qboolean MOBA_TeamHasPlayer( team_t team )
{
	int i;

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( ent->inuse && ent->client &&
			ent->client->sess.sessionTeam == team &&
			ent->client->pers.connected == CON_CONNECTED )
		{
			return qtrue;
		}
	}
	return qfalse;
}

static void MOBA_EnsureTeams( void )
{
	int i;

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( !ent->inuse || !ent->client ||
			ent->client->pers.connected != CON_CONNECTED )
		{
			continue;
		}

		// everybody plays in MOBA, there is no spectator slot: a client that
		// sits in TEAM_FREE or in the team menu would never count as a player
		// and the round start waits for both teams, which deadlocked the draft
		if ( ent->client->sess.sessionTeam != TEAM_RED &&
			ent->client->sess.sessionTeam != TEAM_BLUE )
		{
			SetTeamQuick( ent, PickTeam( i ), qfalse );
		}
	}
}

static void MOBA_RespawnEveryoneOnTeams( void )
{
	int i;

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( !ent->inuse || !ent->client ||
			ent->client->pers.connected != CON_CONNECTED )
		{
			continue;
		}

		if ( ent->client->sess.sessionTeam == TEAM_RED ||
			ent->client->sess.sessionTeam == TEAM_BLUE )
		{
			mobaPlayers[i].dead = qfalse;
			ent->client->respawnTime = 0;
			ClientRespawn( ent );
		}
	}
}

static void MOBA_RefreshAliveCounts( void )
{
	int i;

	mobaRedAlive = 0;
	mobaBlueAlive = 0;
	mobaRedPlayers = 0;
	mobaBluePlayers = 0;

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( !ent->inuse || !ent->client ||
			ent->client->pers.connected != CON_CONNECTED )
		{
			continue;
		}

		if ( ent->client->sess.sessionTeam == TEAM_RED )
		{
			mobaRedPlayers++;
			if ( ent->client->ps.pm_type != PM_DEAD &&
				mobaPlayers[i].dead == qfalse )
			{
				mobaRedAlive++;
			}
		}
		else if ( ent->client->sess.sessionTeam == TEAM_BLUE )
		{
			mobaBluePlayers++;
			if ( ent->client->ps.pm_type != PM_DEAD &&
				mobaPlayers[i].dead == qfalse )
			{
				mobaBlueAlive++;
			}
		}
	}
}

static void MOBA_StartLobby( void )
{
	mobaPhase = MOBA_PHASE_LOBBY;
	mobaPhaseEnd = 0;
	MOBA_CPAll( "^2Magic Wands^7 - waiting for players (2+)\n" );
}

static void MOBA_StartDraft( void )
{
	mobaPhase = MOBA_PHASE_DRAFT;
	mobaPhaseEnd = level.time + moba_pickTime.integer * 1000;

	MOBA_EnsureTeams();
	MOBA_CPAll( "^3Hero select phase!^7  !heroes - list, !pick N - choose (%i sec)\n",
		moba_pickTime.integer );
}

static void MOBA_StartBuy( void )
{
	mobaPhase = MOBA_PHASE_BUY;
	mobaPhaseEnd = level.time + moba_buyTime.integer * 1000;

	MOBA_EnsureTeams();

	// a player who never used !pick still has to shop with a real hero, the
	// placeholder stats have no abilities and are not what the shop prices
	// were balanced against
	MOBA_FillMissingHeroes();
	MOBA_RespawnEveryoneOnTeams();

	// Without a starting stash the shop is unusable in the first buy phase: gold
	// only ever arrives from kills and round results, and kill rewards come later.
	if ( moba_startGold.integer > 0 )
	{
		int gi;

		for ( gi = 0; gi < level.maxclients; gi++ )
		{
			gentity_t *ent = &g_entities[gi];

			if ( !ent->inuse || !ent->client ||
				ent->client->pers.connected != CON_CONNECTED ||
				mobaPlayers[gi].gotStartGold )
			{
				continue;
			}

			mobaPlayers[gi].gotStartGold = qtrue;
			mobaPlayers[gi].gold += moba_startGold.integer;
			MOBA_CPSelf( ent, "^2Starting gold: %i^7\n", moba_startGold.integer );
		}
	}

	MOBA_CPAll( "^3Buy phase:^7 %i sec  press ^3B^7 for the shop panel  !buy N  !upgrade N\n",
		moba_buyTime.integer );
}

static void MOBA_StartFight( void )
{
	mobaPhase = MOBA_PHASE_FIGHT;
	mobaPhaseEnd = 0;

	// the buy phase assigned the heroes already, this only covers a fight that
	// somehow starts without one
	MOBA_FillMissingHeroes();
	MOBA_RespawnEveryoneOnTeams();
	MOBA_CPAll( "^1ROUND %i - FIGHT!^7\n", mobaRound + 1 );
}

static void MOBA_StartRoundEnd( void )
{
	team_t winner = TEAM_NUM_TEAMS;
	int i;

	if ( mobaRedAlive > mobaBlueAlive )
	{
		winner = TEAM_RED;
	}
	else if ( mobaBlueAlive > mobaRedAlive )
	{
		winner = TEAM_BLUE;
	}

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( !ent->inuse || !ent->client ||
			ent->client->pers.connected != CON_CONNECTED )
		{
			continue;
		}

		if ( ent->client->sess.sessionTeam == winner )
		{
			mobaPlayers[i].gold += 700;
			MOBA_CPSelf( ent, "^2Round win! +700 gold\n" );
		}
		else if ( ent->client->sess.sessionTeam == TEAM_RED ||
			ent->client->sess.sessionTeam == TEAM_BLUE )
		{
			mobaPlayers[i].gold += 200;
			MOBA_CPSelf( ent, "^1Round loss. +200 gold\n" );
		}
	}

	mobaRound++;
	mobaPhase = MOBA_PHASE_ROUNDEND;
	mobaPhaseEnd = level.time + moba_roundEndTime.integer * 1000;

	if ( winner == TEAM_RED )
	{
		MOBA_CPAll( "^2Red team wins round %i!^7\n", mobaRound );
	}
	else if ( winner == TEAM_BLUE )
	{
		MOBA_CPAll( "^1Blue team wins round %i!^7\n", mobaRound );
	}
	else
	{
		MOBA_CPAll( "^3Round %i is a draw!^7\n", mobaRound );
	}
}

void MOBA_InitGame( void )
{
	MOBA_LoadHeroes();
	MOBA_ResetPlayers();
	mobaRound = 0;
	mobaPhase = MOBA_PHASE_LOBBY;
	mobaPhaseEnd = 0;

	mobaEnabled = ( g_moba.integer != 0 ) ? qtrue : qfalse;

	if ( mobaEnabled )
	{
		// The mod is built around two teams, so refuse to run in a mode that has
		// none instead of silently behaving like free for all.
		if ( level.gametype != GT_TEAM )
		{
			MOBA_LogLine( va( "MOBA needs a team game, g_gametype %i -> %i",
				level.gametype, GT_TEAM ), NULL );
			level.gametype = GT_TEAM;
			trap->Cvar_Set( "g_gametype", va( "%i", GT_TEAM ) );
		}

		MOBA_CPAll( "^2Magic Wands^7: MOBA mode active (heroes: %i)\n", mobaNumHeroes );
		MOBA_StartLobby();
	}
}

//=========================================================================
// Dev aids - only active when the matching cvar is set, used to exercise the
// mod on a headless server where no real client can type chat commands.
//=========================================================================

// Replays chat commands for every connected player, cycling through a '|'
// separated list once per interval, so the '!' parser can be tested with bots
// only: +set moba_auto "!pick 3|!buy 1|!1"
// The separator cannot be ';' - the engine treats that as a command separator.
static void MOBA_RunAutoCommand( void )
{
	int i;

	if ( !moba_auto.string[0] )
	{
		return;
	}

	// '!' commands are spread over the draft, buy and fight phases
	if ( mobaPhase != MOBA_PHASE_DRAFT && mobaPhase != MOBA_PHASE_BUY &&
		mobaPhase != MOBA_PHASE_FIGHT )
	{
		return;
	}

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];
		char list[256], *entry, *end;
		int count = 1, idx, j;

		if ( !ent->inuse || !ent->client ||
			ent->client->pers.connected != CON_CONNECTED )
		{
			continue;
		}
		if ( level.time < mobaPlayers[i].autoCmdNext )
		{
			continue;
		}

		mobaPlayers[i].autoCmdNext = level.time + moba_autoEvery.integer;

		Q_strncpyz( list, moba_auto.string, sizeof( list ) );

		for ( j = 0; list[j]; j++ )
		{
			if ( list[j] == '|' )
			{
				count++;
			}
		}

		idx = mobaPlayers[i].autoCmdIdx % count;
		mobaPlayers[i].autoCmdIdx = ( idx + 1 ) % count;

		entry = list;
		for ( j = 0; j < idx && entry; j++ )
		{
			entry = strchr( entry, '|' );
			if ( entry )
			{
				entry++;
			}
		}
		if ( !entry )
		{
			continue;
		}

		end = strchr( entry, '|' );
		if ( end )
		{
			*end = '\0';
		}
		while ( *entry == ' ' || *entry == '\t' )
		{
			entry++;
		}
		if ( !*entry )
		{
			continue;
		}

		// accept both "!buy 1" and "buy 1" in the cvar
		MOBA_HandleChat( ent, entry[0] == '!' ? entry : va( "!%s", entry ) );
	}
}

// Repeating lethal hit through the normal damage path, so the kill -> gold/xp ->
// round end chain can be verified without relying on bot aggression. The team of
// the given client is wiped out one member every 2s, which ends the round.
static void MOBA_RunTestKill( void )
{
	static int nextKill = 0;
	int i, j, team;

	if ( mobaPhase != MOBA_PHASE_FIGHT ||
		( moba_testkill.integer < 0 && moba_testkill.integer != -2 ) )
	{
		return;
	}
	if ( level.time < nextKill )
	{
		return;
	}
	nextKill = level.time + 2000;

	// moba_testkill -2: alternate a normal kill with a shot from the dead body,
	// the damage from the dead player has to be refused by
	// MOBA_ShouldBlockDamage while the target keeps its health
	if ( moba_testkill.integer == -2 )
	{
		static int deadShotStep = 0;

		if ( deadShotStep++ % 2 == 0 )
		{
			// fall through to the regular kill below
		}
		else
		{
			for ( i = 0; i < level.maxclients; i++ )
			{
				gentity_t *attacker = &g_entities[i];
				gentity_t *targ = NULL;
				int before;

				if ( !attacker->inuse || !attacker->client ||
					attacker->client->pers.connected != CON_CONNECTED ||
					!mobaPlayers[i].dead )
				{
					continue;
				}

				for ( j = 0; j < level.maxclients; j++ )
				{
					gentity_t *other = &g_entities[j];

					if ( !other->inuse || !other->client ||
						other->client->pers.connected != CON_CONNECTED ||
						other->health <= 0 || OnSameTeam( other, attacker ) )
					{
						continue;
					}
					targ = other;
					break;
				}

				if ( !targ )
				{
					continue;
				}

				before = targ->health;
				MOBA_LogLine( va( "[test] dead %s shoots %s (%i HP)",
					attacker->client->pers.netname, targ->client->pers.netname, before ), NULL );
				G_Damage( targ, attacker, attacker, NULL, targ->client->ps.origin, 50,
					DAMAGE_NO_PROTECTION, MOD_BLASTER );
				MOBA_LogLine( va( "[test] HP %s: %i -> %i %s", targ->client->pers.netname,
					before, targ->health, targ->health == before ? "OK" : "PROBLEM" ), NULL );
				return;
			}
			return;
		}
	}

	team = -1;
	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( ent->inuse && ent->client &&
			ent->client->pers.connected == CON_CONNECTED &&
			i == moba_testkill.integer )
		{
			team = ent->client->sess.sessionTeam;
			break;
		}
	}
	if ( team == -1 )
	{
		return;
	}

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *targ = &g_entities[i];
		gentity_t *attacker = NULL;

		if ( !targ->inuse || !targ->client ||
			targ->client->pers.connected != CON_CONNECTED ||
			targ->client->sess.sessionTeam != team ||
			targ->health <= 0 )
		{
			continue;
		}

		for ( j = 0; j < level.maxclients; j++ )
		{
			gentity_t *other = &g_entities[j];

			if ( !other->inuse || !other->client || other == targ ||
				other->client->pers.connected != CON_CONNECTED ||
				other->health <= 0 || OnSameTeam( other, targ ) )
			{
				continue;
			}
			attacker = other;
			break;
		}

		if ( !attacker )
		{
			continue;
		}

		MOBA_CPAll( "^3[test] %s kills %s^7\n",
			attacker->client->pers.netname, targ->client->pers.netname );
		G_Damage( targ, attacker, attacker, NULL, targ->client->ps.origin, 99999,
			DAMAGE_NO_PROTECTION, MOD_BLASTER );
		return;
	}
}

//=========================================================================
// Drops one client during a fight to exercise the disconnect cleanup:
// +set moba_testkick 1
//=========================================================================
static void MOBA_RunTestKick( void )
{
	static int nextKick = 0;
	int num = moba_testkick.integer;
	gentity_t *ent;

	if ( num < 0 || num >= level.maxclients || mobaPhase != MOBA_PHASE_FIGHT )
	{
		return;
	}
	if ( level.time < nextKick )
	{
		return;
	}
	nextKick = level.time + 1000;

	ent = &g_entities[num];
	if ( !ent->inuse || !ent->client || ent->client->pers.connected != CON_CONNECTED )
	{
		return;
	}

	MOBA_CPAll( "^3[test] disconnect %s (client %i)\n", ent->client->pers.netname, num );
	trap->DropClient( num, "moba smoke test" );
}

//=========================================================================
// Vanilla respawn has to be kept out of the way during a round. The death
// animation in g_combat.c resets respawnTime to now+1s and g_active.c spawns
// the client on any attack button press, both of which happen after the death
// hook, so the timer is pushed away again every frame. Buyback and the next buy
// phase are the only ways back in, and they respawn explicitly.
//=========================================================================
static void MOBA_HoldDeadPlayers( void )
{
	int i;

	if ( !mobaEnabled )
	{
		return;
	}

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( !ent->inuse || !ent->client || !mobaPlayers[i].dead )
		{
			continue;
		}
		if ( ent->client->respawnTime < level.time + 60000 )
		{
			ent->client->respawnTime = level.time + MOBA_FALLBACK_RESPAWN;
		}
	}
}

//=========================================================================
// Fills an empty server with bots so a single player can try the mod out:
// +set moba_bots 3
//=========================================================================
static void MOBA_RunTestBots( void )
{
	static const char *botNames[] = { "Shadowtrooper", "Reborn", "Stormtrooper" };
	static int nextTry = 0;
	static int issued = 0;
	static qboolean done = qfalse;
	int i, want, have = 0;

	if ( done || moba_bots.integer <= 0 || level.time < nextTry )
	{
		return;
	}
	nextTry = level.time + 1000;

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( ent->inuse && ent->client &&
			ent->client->pers.connected == CON_CONNECTED )
		{
			have++;
		}
	}

	// A requested bot only shows up a moment later, so the request counter is
	// what limits the spawning, not the number of connected clients. Asking for
	// one bot per second keeps every bot in its own client slot.
	want = moba_bots.integer + 1;
	if ( have >= want || issued >= moba_bots.integer )
	{
		done = qtrue;
		return;
	}

	MOBA_LogLine( va( "adding bot %s (%i/%i)",
		botNames[issued % ARRAY_LEN( botNames )], issued + 1, moba_bots.integer ), NULL );
	issued++;
	trap->SendConsoleCommand( EXEC_INSERT,
		va( "addbot \"%s\" 2\n", botNames[( issued - 1 ) % ARRAY_LEN( botNames )] ) );
}

// per slot memory of the last pushed state, kept next to the function so a
// disconnect can clear it: a client that lands in a reused slot would
// otherwise start with the previous occupant's state and never be told about
// its gold or its item mask
static char mobaLastSent[MAX_CLIENTS][64];
static qboolean mobaShopStateLogged[MAX_CLIENTS];

//=========================================================================
// Pushes the shop state of a client to its own cgame with a server command:
// "mobaShop phase seconds gold itemMask level". Server commands are used
// instead of a configstring because the engine and the mp gamecode disagree on
// how many configstrings exist, so any slot index above the engine limit would
// be dropped silently. The countdown is sent in whole seconds, the cgame
// continues it locally, and the value is only pushed when it changed.
//=========================================================================
static void MOBA_PushShopState( int clientNum )
{
	gentity_t *ent = &g_entities[clientNum];
	char buf[64];
	int secs;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS ||
		!ent->inuse || !ent->client ||
		ent->client->pers.connected != CON_CONNECTED || !mobaPlayers[clientNum].inuse )
	{
		return;
	}

	secs = ( mobaPhaseEnd > level.time ) ? ( mobaPhaseEnd - level.time + 999 ) / 1000 : 0;
	Com_sprintf( buf, sizeof( buf ), "%i %i %i %i %i",
		mobaPhase, secs, mobaPlayers[clientNum].gold,
		mobaPlayers[clientNum].itemMask, mobaPlayers[clientNum].level );

	if ( Q_stricmp( buf, mobaLastSent[clientNum] ) != 0 )
	{
		Q_strncpyz( mobaLastSent[clientNum], buf, sizeof( mobaLastSent[clientNum] ) );
		trap->SendServerCommand( ent->s.number, va( "mobaShop \"%s\"", buf ) );

		// G_Printf must not be used here: the G_PRINT syscall of this engine
		// kills the game module, so the mod logs through its own G_LogPrintf
		// wrapper like every other MOBA line.
		if ( !mobaShopStateLogged[clientNum] )
		{
			mobaShopStateLogged[clientNum] = qtrue;
			MOBA_LogLine( va( "shop state -> client %i: %s", clientNum, buf ), NULL );
		}
	}
}

void MOBA_RunFrame( void )
{
	int i;

	mobaEnabled = ( g_moba.integer != 0 ) ? qtrue : qfalse;

	if ( !mobaEnabled )
	{
		return;
	}

	MOBA_RefreshAliveCounts();
	MOBA_HoldDeadPlayers();
	MOBA_RunTestBots();
	MOBA_RunAutoCommand();
	MOBA_RunTestKill();
	MOBA_RunTestKick();

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		MOBA_PushShopState( i );
	}

	switch ( mobaPhase )
	{
	case MOBA_PHASE_LOBBY:
		if ( level.numConnectedClients >= 2 )
		{
			MOBA_StartDraft();
		}
		break;

	case MOBA_PHASE_DRAFT:
		if ( level.time >= mobaPhaseEnd )
		{
			if ( MOBA_TeamHasPlayer( TEAM_RED ) && MOBA_TeamHasPlayer( TEAM_BLUE ) )
			{
				MOBA_StartBuy();
			}
			else
			{
				MOBA_CPAll( "^3Waiting for players to start the round...\n" );
				MOBA_StartDraft();
			}
		}
		break;

	case MOBA_PHASE_BUY:
		if ( level.time >= mobaPhaseEnd )
		{
			if ( MOBA_TeamHasPlayer( TEAM_RED ) && MOBA_TeamHasPlayer( TEAM_BLUE ) )
			{
				MOBA_StartFight();
			}
			else
			{
				MOBA_CPAll( "^3Not enough players - back to hero select.\n" );
				MOBA_StartDraft();
			}
		}
		break;

	case MOBA_PHASE_FIGHT:
		MOBA_EnsureTeams();
		if ( mobaRedAlive == 0 || mobaBlueAlive == 0 )
		{
			if ( mobaRedPlayers == 0 || mobaBluePlayers == 0 )
			{
				MOBA_CPAll( "^3Team is empty - waiting for players.\n" );
				MOBA_StartDraft();
			}
			else
			{
				MOBA_StartRoundEnd();
			}
		}
		break;

	case MOBA_PHASE_ROUNDEND:
		if ( level.time >= mobaPhaseEnd )
		{
			if ( MOBA_TeamHasPlayer( TEAM_RED ) && MOBA_TeamHasPlayer( TEAM_BLUE ) )
			{
				MOBA_StartBuy();
			}
			else
			{
				MOBA_StartDraft();
			}
		}
		break;
	}
}

qboolean MOBA_Active( void )
{
	return mobaEnabled;
}

//=========================================================================
// Suicide
//=========================================================================

// G_Kill sets the health to -999 and calls player_die directly, so it never
// passes MOBA_ShouldBlockDamage: without this a player could kill himself during
// the draft or the buy phase and a dead player counts towards the team wipe
// that ends the round.
qboolean MOBA_CanSuicide( gentity_t *ent )
{
	if ( !mobaEnabled || !ent || !ent->client )
	{
		return qtrue;
	}

	if ( mobaPhase != MOBA_PHASE_FIGHT )
	{
		MOBA_CPSelf( ent, "^3No suicide outside the fight.\n" );
		return qfalse;
	}

	return qtrue;
}

//=========================================================================
// Damage integration
//=========================================================================

qboolean MOBA_ShouldBlockDamage( gentity_t *targ, gentity_t *attacker )
{
	if ( !mobaEnabled )
	{
		return qfalse;
	}

	if ( !targ || !targ->client )
	{
		return qfalse;
	}

	if ( targ->client->sess.sessionTeam == TEAM_SPECTATOR )
	{
		return qtrue;
	}

	// outside the fight nobody can be hurt, that covers the draft, the buy
	// phase and the round end
	if ( mobaPhase != MOBA_PHASE_FIGHT )
	{
		return qtrue;
	}

	if ( attacker && attacker->client &&
		attacker->client->sess.sessionTeam != TEAM_SPECTATOR &&
		OnSameTeam( targ, attacker ) )
	{
		return qtrue;
	}

	// A player waiting for a buyback must not keep fighting from the grave.
	if ( attacker && attacker->client && attacker->s.number >= 0 &&
		attacker->s.number < MAX_CLIENTS &&
		mobaPlayers[attacker->s.number].dead )
	{
		MOBA_LogLine( va( "damage from dead %s to %s blocked",
			attacker->client->pers.netname, targ->client->pers.netname ), NULL );
		return qtrue;
	}

	return qfalse;
}

int MOBA_AdjustDamage( gentity_t *targ, gentity_t *attacker, int damage )
{
	mobaPlayer_t *p;
	float mult;

	// vehicles and other scripted entities carry a client pointer but do not
	// sit in a client slot, they must never index the player array
	if ( !mobaEnabled || !attacker || !attacker->client || targ == attacker ||
		attacker->s.number < 0 || attacker->s.number >= MAX_CLIENTS )
	{
		return damage;
	}

	p = &mobaPlayers[attacker->s.number];

	if ( p->buffEndTime > level.time )
	{
		mult = p->dmgMult * p->buffMult;
	}
	else
	{
		// an expired buff keeps its multiplier in the struct, reset it here so
		// the value the player sees in !status matches what the damage does
		if ( p->buffMult != 1.0f )
		{
			p->buffMult = 1.0f;
		}

		mult = p->dmgMult;
	}

	if ( mult > 1.01f )
	{
		damage = (int)( damage * mult );
	}

	if ( damage < 1 )
	{
		damage = 1;
	}

	return damage;
}

//=========================================================================
// Hero stats
//=========================================================================

static void MOBA_ApplyHeroStats( gentity_t *ent )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	const mobaHero_t *h;
	int health, armor, dmgBonus = 0, i;

	if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR )
	{
		return;
	}

	if ( p->heroId < 0 )
	{
		ent->client->ps.stats[STAT_HEALTH] = MOBA_DEFAULT_MAXHEALTH;
		ent->client->ps.stats[STAT_MAX_HEALTH] = MOBA_DEFAULT_MAXHEALTH;
		ent->client->ps.stats[STAT_ARMOR] = MOBA_DEFAULT_ARMOR;
		return;
	}

	h = &mobaHeroes[p->heroId];

	health = h->baseHealth + p->level * h->healthPerLevel;
	armor = h->baseArmor;

	for ( i = 0; i < mobaNumItems; i++ )
	{
		if ( p->itemMask & ( 1 << i ) )
		{
			health += mobaItems[i].healthBonus;
			armor += mobaItems[i].armorBonus;
			dmgBonus += mobaItems[i].dmgBonusPercent;
		}
	}

	if ( health > MOBA_MAX_HEALTH )
	{
		health = MOBA_MAX_HEALTH;
	}
	if ( armor > MOBA_MAX_ARMOR )
	{
		armor = MOBA_MAX_ARMOR;
	}
	if ( armor < 0 )
	{
		armor = 0;
	}

	p->dmgMult = 1.0f + dmgBonus / 100.0f;
	if ( p->dmgMult < 1.0f )
	{
		p->dmgMult = 1.0f;
	}

	ent->client->ps.stats[STAT_HEALTH] = health;
	ent->client->ps.stats[STAT_MAX_HEALTH] = health;
	ent->client->ps.stats[STAT_ARMOR] = armor;

	// G_Damage copies ent->health back into the stat every time, so writing the
	// stat alone leaves the HUD and the real health disagreeing after a level up
	ent->health = health;
}

void MOBA_OnClientSpawn( gentity_t *ent )
{
	if ( !mobaEnabled || !ent->client )
	{
		return;
	}

	mobaPlayers[ent->s.number].inuse = qtrue;
	mobaPlayers[ent->s.number].dead = qfalse;
	MOBA_ApplyHeroStats( ent );

	// A player who never saw the manual has no way to find the commands, so the
	// hint goes out once per connection instead of waiting for !help.
	if ( !mobaPlayers[ent->s.number].greeted )
	{
		mobaPlayers[ent->s.number].greeted = qtrue;
		MOBA_CPSelf( ent, "^3Magic Wands^7: !pick N - hero,\n"
			"^3B^7 - open the shop panel in the buy phase,\n"
			"^3G H J N X ;^7 - buy an item, ^3B^7 - close it,\n"
			"!buy N|code - buy, !buyall - buy everything affordable,\n"
			"!upgrade N - upgrade ability, !1-!4 - abilities,\n"
			"!buyback - return after death, !status - stats, !help - all commands\n" );
	}
}

void MOBA_OnClientDisconnect( gentity_t *ent )
{
	if ( !ent || !ent->client )
	{
		return;
	}

	// Without this the slot keeps its hero, gold and levels, so a reconnecting
	// player silently resumes the previous run instead of starting clean.
	if ( mobaEnabled && ent->s.number >= 0 && ent->s.number < MAX_CLIENTS )
	{
		memset( &mobaPlayers[ent->s.number], 0, sizeof( mobaPlayers[ent->s.number] ) );
		mobaPlayers[ent->s.number].heroId = -1;
		mobaPlayers[ent->s.number].autoCmdNext = 0;
		mobaPlayers[ent->s.number].autoCmdIdx = 0;
		mobaLastSent[ent->s.number][0] = '\0';
		mobaShopStateLogged[ent->s.number] = qfalse;
		MOBA_LogLine( "player disconnected, slot cleared", ent );
	}
}

//=========================================================================
// Death / economy
//=========================================================================

static void MOBA_CheckLevelUp( gentity_t *ent, int gainedXp )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	int need = p->level * 100 + 50;

	p->xp += gainedXp;

	while ( p->xp >= need )
	{
		p->xp -= need;
		p->level++;
		p->skillPoints++;
		need = p->level * 100 + 50;

		MOBA_CPSelf( ent, "^2Level %i!^7 +1 skill point ( !upgrade )\n", p->level );
		MOBA_ApplyHeroStats( ent );
	}
}

void MOBA_OnPlayerDeath( gentity_t *self, gentity_t *attacker, int meansOfDeath )
{
	mobaPlayer_t *vp, *kp;
	int xpGain, goldGain;

	if ( !mobaEnabled || !self->client ||
		self->s.number < 0 || self->s.number >= MAX_CLIENTS )
	{
		return;
	}

	vp = &mobaPlayers[self->s.number];
	vp->dead = qtrue;

	// never auto-respawn during a fight round
	self->client->respawnTime = level.time + MOBA_FALLBACK_RESPAWN;

	// vehicles and scripted entities have a client pointer but no client slot
	if ( attacker && attacker->client &&
		attacker->s.number >= 0 && attacker->s.number < MAX_CLIENTS &&
		attacker != self &&
		OnSameTeam( self, attacker ) == qfalse )
	{
		kp = &mobaPlayers[attacker->s.number];

		goldGain = MOBA_GOLD_KILL;
		kp->gold += goldGain;
		kp->roundKills++;

		// the reward follows the level of the hero that was killed, farming a
		// weak hero pays less than taking on a developed one
		xpGain = MOBA_XP_KILL_BASE + vp->level * MOBA_XP_KILL_PER_LEVEL;

		MOBA_CheckLevelUp( attacker, xpGain );

		MOBA_CPSelf( attacker, "^2+%i gold and +%i xp for the kill!^7\n", goldGain, xpGain );
		MOBA_CPSelf( self, "^1You died. !buyback to return (%i gold)\n",
			100 + vp->level * 100 );
	}
	else
	{
		MOBA_CPSelf( self, "^1You died. !buyback to return (%i gold)\n",
			100 + vp->level * 100 );
	}
}

//=========================================================================
// Spawn points
//=========================================================================

gentity_t *MOBA_PickSpawnPoint( gentity_t *ent, vec3_t origin, vec3_t angles )
{
	const char *spawnClass = NULL;
	gentity_t *spot = NULL, *ents[32];
	int count = 0, i;
	team_t team = ent->client->sess.sessionTeam;

	if ( mobaPhase == MOBA_PHASE_FIGHT || mobaPhase == MOBA_PHASE_BUY )
	{
		if ( team == TEAM_RED )
		{
			spawnClass = "team_CTF_redspawn";
		}
		else if ( team == TEAM_BLUE )
		{
			spawnClass = "team_CTF_bluespawn";
		}
	}

	if ( spawnClass )
	{
		while ( ( spot = G_Find( spot, FOFS( classname ), spawnClass ) ) != NULL )
		{
			if ( count < ARRAY_LEN( ents ) )
			{
				ents[count++] = spot;
			}
		}

		if ( count > 0 )
		{
			spot = ents[Q_irand( 0, count - 1 )];
			VectorCopy( spot->s.origin, origin );
			VectorCopy( spot->s.angles, angles );
			angles[PITCH] = 0;
			return spot;
		}
	}

	return SelectSpawnPoint( ent->client->ps.origin, origin, angles, team, qfalse );
}

//=========================================================================
// Abilities
//=========================================================================

static qboolean MOBA_IsHostile( gentity_t *caster, gentity_t *targ )
{
	if ( !targ->inuse || !targ->client )
	{
		return qfalse;
	}
	if ( targ->client->sess.sessionTeam == TEAM_SPECTATOR )
	{
		return qfalse;
	}
	if ( targ->client->ps.pm_type == PM_DEAD )
	{
		return qfalse;
	}
	if ( OnSameTeam( caster, targ ) )
	{
		return qfalse;
	}
	return qtrue;
}

static qboolean MOBA_IsAlly( gentity_t *caster, gentity_t *targ )
{
	if ( !targ->inuse || !targ->client )
	{
		return qfalse;
	}
	if ( targ->client->sess.sessionTeam == TEAM_SPECTATOR )
	{
		return qfalse;
	}
	if ( targ->client->ps.pm_type == PM_DEAD )
	{
		return qfalse;
	}
	if ( !OnSameTeam( caster, targ ) )
	{
		return qfalse;
	}
	return qtrue;
}

static void MOBA_CastDirect( gentity_t *ent, const mobaAbility_t *ab )
{
	trace_t tr;
	vec3_t fwd, start, end;
	gentity_t *target;
	int dmg = MOBA_AbilityPower( ent, ab );

	AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
	VectorCopy( ent->client->ps.origin, start );
	start[2] += 36;
	VectorMA( start, ab->range, fwd, end );

	trap->Trace( &tr, start, NULL, NULL, end, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0 );

	if ( tr.entityNum >= 0 && tr.entityNum < MAX_CLIENTS && tr.entityNum != ent->s.number )
	{
		target = &g_entities[tr.entityNum];

		if ( MOBA_IsHostile( ent, target ) )
		{
			G_Damage( target, ent, ent, fwd, tr.endpos, dmg, 0, MOD_MOBA );
			MOBA_CPSelf( ent, "^5%s:^7 %i damage to %s\n",
				ab->name, dmg, target->client->pers.netname );
		}
		else
		{
			MOBA_CPSelf( ent, "Missed, hit %s\n", target->client ? target->client->pers.netname : "an ally" );
		}
	}
	else
	{
		MOBA_CPSelf( ent, "No target within %i!\n", (int)ab->range );
	}
}

static void MOBA_CastAoEDamage( gentity_t *ent, const mobaAbility_t *ab )
{
	int i, dmg = MOBA_AbilityPower( ent, ab );

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *target = &g_entities[i];

		if ( target->inuse && target->client && MOBA_IsHostile( ent, target ) )
		{
			vec3_t dir;

			VectorSubtract( target->client->ps.origin, ent->client->ps.origin, dir );

			if ( VectorLength( dir ) <= ab->radius )
			{
				G_Damage( target, ent, ent, dir, target->client->ps.origin, dmg, 0, MOD_MOBA );
			}
		}
	}

	MOBA_CPSelf( ent, "%s - %i area damage\n", ab->name, dmg );
}

static void MOBA_CastAoEHeal( gentity_t *ent, const mobaAbility_t *ab )
{
	int i, heal = MOBA_AbilityPower( ent, ab );

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *target = &g_entities[i];

		if ( target->inuse && target->client && MOBA_IsAlly( ent, target ) )
		{
			vec3_t dir;
			int cur, max;

			VectorSubtract( target->client->ps.origin, ent->client->ps.origin, dir );

			if ( VectorLength( dir ) <= ab->radius )
			{
				cur = target->client->ps.stats[STAT_HEALTH];
				max = target->client->ps.stats[STAT_MAX_HEALTH];
				cur += heal;
				if ( cur > max )
				{
					cur = max;
				}
				target->client->ps.stats[STAT_HEALTH] = cur;
				target->health = cur;
			}
		}
	}

	MOBA_CPSelf( ent, "%s - %i area heal\n", ab->name, heal );
}

static void MOBA_CastBuff( gentity_t *ent, const mobaAbility_t *ab )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	qboolean active = ( p->buffEndTime > level.time ) ? qtrue : qfalse;

	// a weaker buff must not eat a stronger one that is still running, the
	// duration of the running buff is extended instead
	if ( active && p->buffMult >= ab->buffMult )
	{
		if ( p->buffEndTime < level.time + ab->durationMs )
		{
			p->buffEndTime = level.time + ab->durationMs;
		}

		MOBA_CPSelf( ent, "%s! Damage stays x%.0f\n", ab->name, p->buffMult );
		return;
	}

	p->buffMult = ab->buffMult;
	p->buffEndTime = level.time + ab->durationMs;

	MOBA_CPSelf( ent, "%s! Damage x%.0f for %.1f sec\n",
		ab->name, ab->buffMult, ab->durationMs / 1000.0f );
}

// ability power scales with the rank the player bought
static int MOBA_AbilityRank( gentity_t *ent, const mobaAbility_t *ab )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	int slot = (int)( ab - mobaHeroes[p->heroId].abilities );

	if ( slot < 0 || slot >= MOBA_ABILITIES_PER_HERO )
	{
		slot = 0;
	}

	if ( p->abilityLevel[slot] < 1 )
	{
		return 1;
	}

	return p->abilityLevel[slot];
}

static int MOBA_AbilityPower( gentity_t *ent, const mobaAbility_t *ab )
{
	return ab->baseEffect + ab->perLevelEffect * MOBA_AbilityRank( ent, ab );
}

static void MOBA_CastAbility( gentity_t *ent, int slot )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	const mobaHero_t *h;
	const mobaAbility_t *ab;

	if ( mobaPhase != MOBA_PHASE_FIGHT )
	{
		MOBA_CPSelf( ent, "Abilities only work in the fight phase!\n" );
		return;
	}

	if ( p->heroId < 0 )
	{
		MOBA_CPSelf( ent, "Pick a hero first ( !pick N )!\n" );
		return;
	}

	if ( p->dead )
	{
		MOBA_CPSelf( ent, "You are dead!\n" );
		return;
	}

	if ( ent->client->ps.pm_type == PM_DEAD )
	{
		return;
	}

	if ( slot < 0 || slot >= MOBA_ABILITIES_PER_HERO )
	{
		return;
	}

	h = &mobaHeroes[p->heroId];
	ab = &h->abilities[slot];

	if ( level.time < p->cdReady[slot] )
	{
		MOBA_CPSelf( ent, "%s on cooldown (%i sec)\n",
			ab->name, ( p->cdReady[slot] - level.time ) / 1000 + 1 );
		return;
	}

	switch ( ab->type )
	{
	case AB_DIRECT:
		MOBA_CastDirect( ent, ab );
		break;
	case AB_AOE_DAMAGE:
		MOBA_CastAoEDamage( ent, ab );
		break;
	case AB_AOE_HEAL:
		MOBA_CastAoEHeal( ent, ab );
		break;
	case AB_BUFF:
		MOBA_CastBuff( ent, ab );
		break;
	default:
		return;
	}

	p->cdReady[slot] = level.time + ab->cooldownMs;
}

//=========================================================================
// Chat commands (players type !command)
//=========================================================================

static void MOBA_ListHeroes( gentity_t *ent )
{
	int i;

	for ( i = 0; i < mobaNumHeroes; i++ )
	{
		MOBA_Self( ent, "^3%2i^7 - %s (^5%s^7)",
			i + 1, mobaHeroes[i].name, mobaHeroes[i].role );
	}
}

//=========================================================================
// Resolves a shop argument to an item index. A number is taken as the item
// number of the on screen panel, anything else is matched against the short
// code of an item. An empty argument is a mistyped command, there is no text
// menu left to carry a cursor.
//=========================================================================
static int MOBA_ResolveItem( gentity_t *ent, const char *arg )
{
	int i;

	if ( arg == NULL || arg[0] == '\0' )
	{
		return -3;
	}

	if ( Q_isalpha( (unsigned char)arg[0] ) == 0 )
	{
		return atoi( arg ) - 1;
	}

	for ( i = 0; i < mobaNumItems; i++ )
	{
		if ( Q_stricmp( arg, mobaItems[i].code ) == 0 )
		{
			return i;
		}
	}

	return -2;		// not a number and not a known code
}

//=========================================================================
// Buys every item the player can still afford, cheapest first, so that a
// single command fills out a round instead of one command per item.
//=========================================================================
static void MOBA_BuyAll( gentity_t *ent )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	qboolean bought[MOBA_MAX_ITEMS];
	int i, j, boughtCount = 0;

	memset( bought, 0, sizeof( bought ) );

	for ( j = 0; j < mobaNumItems; j++ )
	{
		for ( i = 0; i < mobaNumItems; i++ )
		{
			if ( bought[i] || ( p->itemMask & ( 1 << i ) ) )
			{
				continue;
			}
			if ( p->gold < mobaItems[i].price )
			{
				continue;
			}
			MOBA_BuyItem( ent, i );
			if ( p->itemMask & ( 1 << i ) )
			{
				bought[i] = qtrue;
				boughtCount++;
			}
		}
	}

	if ( boughtCount == 0 )
	{
		MOBA_Self( ent, "^3Nothing bought: not enough gold for anything new (you have %i gold)", p->gold );
	}
	else
	{
		MOBA_Self( ent, "^2Buyall:^7 %i item(s) bought, ^2%i^7 gold left", boughtCount, p->gold );
	}
}

static void MOBA_BuyItem( gentity_t *ent, int id )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];

	if ( mobaPhase != MOBA_PHASE_BUY )
	{
		MOBA_CPSelf( ent, "Buying is only allowed in the buy phase!\n" );
		return;
	}

	if ( id == -2 )
	{
		MOBA_Self( ent, "^3No item with that code. Codes: armor, med, rage, heavy, crystal, cloak" );
		return;
	}

	if ( id == -3 )
	{
		MOBA_Self( ent, "^3Give an item number or code, for example !buy 1" );
		return;
	}

	if ( id < 0 || id >= mobaNumItems )
	{
		MOBA_Self( ent, "^3Bad item number, the shop has 1-%i", mobaNumItems );
		return;
	}

	if ( p->itemMask & ( 1 << id ) )
	{
		MOBA_CPSelf( ent, "You already have that item!\n" );
		return;
	}

	if ( p->gold < mobaItems[id].price )
	{
		MOBA_CPSelf( ent, "Not enough gold (%i/%i)!\n",
			p->gold, mobaItems[id].price );
		return;
	}

	p->gold -= mobaItems[id].price;
	p->itemMask |= ( 1 << id );

	MOBA_CPSelf( ent, "Bought: %s! %i gold left\n",
		mobaItems[id].name, p->gold );
}

static void MOBA_UpgradeAbility( gentity_t *ent, int num )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];

	if ( mobaPhase != MOBA_PHASE_BUY )
	{
		MOBA_CPSelf( ent, "Ability upgrades are only allowed in the buy phase!\n" );
		return;
	}

	if ( num < 1 || num > MOBA_ABILITIES_PER_HERO )
	{
		MOBA_CPSelf( ent, "Ability number must be 1-%i!\n", MOBA_ABILITIES_PER_HERO );
		return;
	}

	if ( p->heroId < 0 )
	{
		MOBA_CPSelf( ent, "Pick a hero first!\n" );
		return;
	}

	if ( p->skillPoints <= 0 )
	{
		MOBA_CPSelf( ent, "No skill points!\n" );
		return;
	}

	if ( p->abilityLevel[num - 1] >= MOBA_MAX_SKILL_LEVEL )
	{
		MOBA_CPSelf( ent, "Ability is already maxed out!\n" );
		return;
	}

	p->abilityLevel[num - 1]++;
	p->skillPoints--;

	MOBA_CPSelf( ent, "%s upgraded to level %i!\n",
		mobaHeroes[p->heroId].abilities[num - 1].name,
		p->abilityLevel[num - 1] );
}

static void MOBA_Buyback( gentity_t *ent )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	int cost;

	if ( mobaPhase != MOBA_PHASE_FIGHT )
	{
		MOBA_CPSelf( ent, "Buyback only works in the fight phase!\n" );
		return;
	}

	if ( !p->dead || ent->client->ps.pm_type != PM_DEAD )
	{
		MOBA_CPSelf( ent, "You are still alive!\n" );
		return;
	}

	cost = 100 + p->level * 100;

	if ( p->gold < cost )
	{
		MOBA_CPSelf( ent, "Buyback costs %i gold, you have %i!\n", cost, p->gold );
		return;
	}

	p->gold -= cost;
	p->dead = qfalse;
	ent->client->respawnTime = 0;

	ClientRespawn( ent );

	MOBA_CPSelf( ent, "Bought back for %i gold! Fight!\n", cost );
}

static void MOBA_ShowStatus( gentity_t *ent )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	int i, need = p->level * 100 + 50;

	if ( p->heroId >= 0 )
	{
		MOBA_Self( ent, "Hero: %s (%s), level %i",
			mobaHeroes[p->heroId].name, mobaHeroes[p->heroId].role, p->level );
	}
	else
	{
		MOBA_Self( ent, "No hero picked, level %i", p->level );
	}

	MOBA_Self( ent, "XP: %i / %i, skill points: %i", p->xp, need, p->skillPoints );
	MOBA_Self( ent, "Gold: %i", p->gold );

	if ( p->roundKills > 0 )
	{
		MOBA_Self( ent, "Kills this round: %i", p->roundKills );
	}

	if ( p->heroId >= 0 )
	{
		for ( i = 0; i < MOBA_ABILITIES_PER_HERO; i++ )
		{
			MOBA_Self( ent, "%i. %s (lvl %i)%s",
				i + 1, mobaHeroes[p->heroId].abilities[i].name,
				p->abilityLevel[i], ( level.time < p->cdReady[i] ) ? " ^3(CD)" : "" );
		}
	}

	if ( p->dmgMult > 1.01f )
	{
		MOBA_Self( ent, "Item damage bonus: x%.2f", p->dmgMult );
	}

	if ( p->buffEndTime > level.time )
	{
		MOBA_Self( ent, "Active buff: x%.2f (%.0f sec)",
			p->buffMult, ( p->buffEndTime - level.time ) / 1000 );
	}
}

qboolean MOBA_HandleChat( gentity_t *ent, const char *msg )
{
	char cmd[64], arg1[64];
	const char *p;

	if ( !mobaEnabled || !msg || msg[0] != '!' )
	{
		return qfalse;
	}

	p = msg;

	// extract first word
	cmd[0] = '\0';
	arg1[0] = '\0';
	{
		int i = 0;
		while ( p[i] && p[i] != ' ' && p[i] != '\t' && i < (int)sizeof( cmd ) - 1 )
		{
			cmd[i] = p[i];
			i++;
		}
		cmd[i] = '\0';
		while ( p[i] == ' ' || p[i] == '\t' )
		{
			i++;
		}
		Q_strncpyz( arg1, p + i, sizeof( arg1 ) );
	}

	if ( !Q_stricmp( cmd, "!help" ) )
	{
		MOBA_Self( ent, "^3Commands:^7 !heroes !pick N !buy N|code !buyall !upgrade N !buyback "
			"!1-!4 (abilities) !status\n"
			"^3The shop is the on screen panel:^7 press ^3B^7 in the buy phase, "
			"^3G H J N X ;^7 buys an item, ^3B^7 closes it" );
		return qtrue;
	}
	if ( !Q_stricmp( cmd, "!heroes" ) )
	{
		MOBA_ListHeroes( ent );
		return qtrue;
	}
	if ( !Q_stricmp( cmd, "!buyall" ) )
	{
		if ( mobaPhase != MOBA_PHASE_BUY )
		{
			MOBA_Self( ent, "^3The shop opens in the buy phase, right after the draft." );
			return qtrue;
		}
		MOBA_BuyAll( ent );
		return qtrue;
	}
	if ( !Q_stricmp( cmd, "!status" ) )
	{
		MOBA_ShowStatus( ent );
		return qtrue;
	}

	if ( !Q_stricmp( cmd, "!pick" ) )
	{
		if ( mobaPhase != MOBA_PHASE_DRAFT )
		{
			MOBA_CPSelf( ent, "Heroes can only be picked in the draft phase!\n" );
			return qtrue;
		}
		if ( mobaPlayers[ent->s.number].heroId >= 0 )
		{
			MOBA_CPSelf( ent, "Hero already picked!\n" );
			return qtrue;
		}

		{
			int id = atoi( arg1 ) - 1;
			if ( id < 0 || id >= mobaNumHeroes )
			{
				MOBA_CPSelf( ent, "Bad hero number! See: !heroes\n" );
				return qtrue;
			}

			// one hero per player, a shared hero would stack two players' worth of
			// bonuses on one body and leave the enemy team one hero short
			if ( MOBA_HeroTaken( id ) )
			{
				MOBA_CPSelf( ent, "%s is already taken! See: !heroes\n",
					mobaHeroes[id].name );
				return qtrue;
			}

			mobaPlayers[ent->s.number].heroId = id;
			MOBA_CPSelf( ent, "Hero picked: ^5%s^7!\n", mobaHeroes[id].name );
		}
		return qtrue;
	}

	if ( !Q_stricmp( cmd, "!buy" ) )
	{
		MOBA_BuyItem( ent, MOBA_ResolveItem( ent, arg1 ) );
		return qtrue;
	}

	if ( !Q_stricmp( cmd, "!upgrade" ) )
	{
		MOBA_UpgradeAbility( ent, atoi( arg1 ) );
		return qtrue;
	}

	if ( !Q_stricmp( cmd, "!buyback" ) )
	{
		MOBA_Buyback( ent );
		return qtrue;
	}

	if ( cmd[0] == '!' && ( cmd[1] >= '1' && cmd[1] <= '4' ) && cmd[2] == '\0' )
	{
		MOBA_CastAbility( ent, cmd[1] - '1' );
		return qtrue;
	}

	return qfalse;
}

//=========================================================================
// Heroes - content table
//=========================================================================

static void MOBA_LoadHeroes( void )
{
	static const mobaHero_t heroes[MOBA_MAX_HEROES] = {
		{ "Ash'Lar",		"Tank",		950, 50, 100, 30, 2,
		{ AB( "Piercing Blade",	AB_DIRECT,		6000,	90,	20,	900,	0,	0,	1.0f, "Hit target" ),
		  AB( "Blade Wall",		AB_AOE_DAMAGE,	12000,	70,	15,	0,		450,	0,	1.0f, "Sweeping strike" ),
		  AB( "Battle Rage",		AB_BUFF,		20000,	0,	0,	0,		0,		9000,	1.6f, "+60% dmg 9s" ),
		  AB( "Earth Rift",		AB_AOE_DAMAGE,	30000,	150,	30,	0,		600,	0,	1.0f, "Powerful shock" ) } },

		{ "Vectar",		"Tank",		920, 48, 100, 28, 2,
		{ AB( "Thunder Hammer",		AB_DIRECT,		7000,	95,	18,	800,	0,	0,	1.0f, "Stunning blow" ),
		  AB( "Stone Skin",		AB_BUFF,		18000,	0,	0,	0,		0,		10000,	1.35f, "+35% dmg 10s" ),
		  AB( "Shockwave",			AB_AOE_DAMAGE,	11000,	60,	12,	0,		500,	0,	1.0f, "Area blast" ),
		  AB( "Golem Wrath",		AB_AOE_DAMAGE,	28000,	130,	25,	0,		550,	0,	1.0f, "Shatter" ) } },

		{ "T'Raine",		"Tank",		980, 55, 100, 26, 2,
		{ AB( "Spike",				AB_DIRECT,		6500,	85,	16,	850,	0,	0,	1.0f, "Piercing thrust" ),
		  AB( "Iron Ring",		AB_AOE_DAMAGE,	12000,	65,	14,	0,		450,	0,	1.0f, "Ring of damage" ),
		  AB( "Unyielding",		AB_BUFF,		22000,	0,	0,	0,		0,		8000,	1.5f, "+50% dmg 8s" ),
		  AB( "Meat Grinder",			AB_AOE_DAMAGE,	32000,	160,	35,	0,		500,	0,	1.0f, "Whirling storm" ) } },

		{ "Korkhin",		"Mage",		560, 28, 50, 22, 2,
		{ AB( "Lightning Lash",		AB_DIRECT,		6000,	110,	25,	1000,	0,	0,	1.0f, "Lightning at target" ),
		  AB( "Fireball",		AB_AOE_DAMAGE,	10000,	85,	18,	0,		400,	0,	1.0f, "Explosion around self" ),
		  AB( "Arc Chain",			AB_DIRECT,		9000,	125,	22,	950,	0,	0,	1.0f, "Strong discharge" ),
		  AB( "Storm",				AB_AOE_DAMAGE,	35000,	200,	45,	0,		600,	0,	1.0f, "Area storm" ) } },

		{ "Silvara",		"Mage",		540, 26, 50, 20, 2,
		{ AB( "Ice Dagger",		AB_DIRECT,		5500,	105,	24,	1000,	0,	0,	1.0f, "Ice at target" ),
		  AB( "Frost Breath",	AB_BUFF,		16000,	0,	0,	0,		0,		7000,	1.4f, "+40% dmg 7s" ),
		  AB( "Hail",				AB_AOE_DAMAGE,	11000,	75,	16,	0,		450,	0,	1.0f, "Hail around" ),
		  AB( "Eternal Winter",		AB_AOE_DAMAGE,	34000,	190,	40,	0,		550,	0,	1.0f, "Freezing storm" ) } },

		{ "Merek",		"Mage",		580, 30, 55, 24, 2,
		{ AB( "Spirit Fire",			AB_DIRECT,		6500,	115,	20,	950,	0,	0,	1.0f, "Flaming beam" ),
		  AB( "Fire Cocktail",			AB_AOE_DAMAGE,	10000,	80,	20,	0,		420,	0,	1.0f, "Explosion" ),
		  AB( "Flame Shield",		AB_BUFF,		20000,	0,	0,	0,		0,		9000,	1.55f, "+55% dmg 9s" ),
		  AB( "Ash Rain",	AB_AOE_DAMAGE,	33000,	195,	42,	0,		580,	0,	1.0f, "Firestorm" ) } },

		{ "Ornat",		"Mage",		520, 25, 50, 22, 2,
		{ AB( "Acid Shot",	AB_DIRECT,		6000,	120,	28,	1000,	0,	0,	1.0f, "Acid" ),
		  AB( "Rot Wave",		AB_AOE_DAMAGE,	11000,	70,	15,	0,		430,	0,	1.0f, "Rot around" ),
		  AB( "Evil Eye",				AB_BUFF,		17000,	0,	0,	0,		0,		8000,	1.45f, "+45% dmg 8s" ),
		  AB( "Plague Column",		AB_AOE_DAMAGE,	36000,	210,	50,	0,		600,	0,	1.0f, "Giant plague" ) } },

		{ "Zum'Zar",		"Mage",		550, 27, 55, 22, 2,
		{ AB( "Thunder Strike",		AB_DIRECT,		7000,	130,	26,	900,	0,	0,	1.0f, "Thunder" ),
		  AB( "Thunderclap",			AB_AOE_DAMAGE,	11000,	78,	17,	0,		480,	0,	1.0f, "Shock wave" ),
		  AB( "Energy Charge",		AB_BUFF,		19000,	0,	0,	0,		0,		8000,	1.5f, "+50% dmg 8s" ),
		  AB( "Thunder Burst",		AB_AOE_DAMAGE,	32000,	180,	38,	0,		560,	0,	1.0f, "Sky rupture" ) } },

		{ "Killian",		"Carry",	620, 32, 60, 35, 3,
		{ AB( "Precision Shot",		AB_DIRECT,		5000,	100,	22,	1100,	0,	0,	1.0f, "Shot" ),
		  AB( "Rapid Fire",	AB_BUFF,		14000,	0,	0,	0,		0,		6000,	1.5f, "+50% dmg 6s" ),
		  AB( "Shrapnel",			AB_AOE_DAMAGE,	10000,	65,	14,	0,		380,	0,	1.0f, "Shards" ),
		  AB( "Golden Bullet",		AB_DIRECT,		26000,	230,	50,	1200,	0,	0,	1.0f, "Lethal shot" ) } },

		{ "Dinara",		"Carry",	600, 30, 65, 38, 3,
		{ AB( "Twin Blades",		AB_DIRECT,		5500,	95,	20,	1000,	0,	0,	1.0f, "Double strike" ),
		  AB( "Blade Dance",		AB_AOE_DAMAGE,	12000,	70,	15,	0,		420,	0,	1.0f, "Ring of blades" ),
		  AB( "Blades of Greed",	AB_BUFF,		18000,	0,	0,	0,		0,		9000,	1.45f, "+45% dmg 9s" ),
		  AB( "Hidden Slash",	AB_DIRECT,		28000,	210,	45,	1000,	0,	0,	1.0f, "Cutting sweep" ) } },

		{ "Starr",		"Carry",	640, 34, 70, 36, 3,
		{ AB( "Assault Volley",		AB_DIRECT,		5000,	85,	18,	1050,	0,	0,	1.0f, "Volley" ),
		  AB( "Roaring Barrage",		AB_AOE_DAMAGE,	11000,	60,	15,	0,		400,	0,	1.0f, "Wave" ),
		  AB( "Adrenaline",			AB_BUFF,		15000,	0,	0,	0,		0,		7000,	1.55f, "+55% dmg 7s" ),
		  AB( "Burst Rounds",	AB_DIRECT,		27000,	220,	48,	1150,	0,	0,	1.0f, "Burst" ) } },

		{ "Brock",		"Carry",	660, 35, 70, 34, 3,
		{ AB( "Chopping Blow",		AB_DIRECT,		6000,	90,	19,	950,	0,	0,	1.0f, "Axe" ),
		  AB( "Whirl",				AB_AOE_DAMAGE,	11000,	65,	14,	0,		420,	0,	1.0f, "Axe whirlwind" ),
		  AB( "Beast Rage",	AB_BUFF,		17000,	0,	0,	0,		0,		8000,	1.5f, "+50% dmg 8s" ),
		  AB( "Crusher",		AB_DIRECT,		30000,	240,	55,	1000,	0,	0,	1.0f, "All-out strike" ) } },

		{ "Lira",		"Healer",	600, 30, 60, 22, 1,
		{ AB( "Light Discipline",	AB_AOE_HEAL,	6000,	80,	15,	0,		600,	0,	1.0f, "Area heal" ),
		  AB( "Ray of Hope",		AB_DIRECT,		8000,	70,	14,	950,	0,	0,	1.0f, "Beam" ),
		  AB( "Blessing",		AB_BUFF,		18000,	0,	0,	0,		0,		9000,	1.4f, "+40% dmg 9s" ),
		  AB( "Greater Heal",	AB_AOE_HEAL,	26000,	220,	40,	0,		700,	0,	1.0f, "Powerful heal" ) } },

		{ "Selena",		"Healer",	580, 28, 55, 20, 1,
		{ AB( "Wave of Life",		AB_AOE_HEAL,	6500,	75,	14,	0,		550,	0,	1.0f, "Heal" ),
		  AB( "Light Spear",		AB_DIRECT,		7000,	80,	16,	1000,	0,	0,	1.0f, "Spear" ),
		  AB( "Inspiration",		AB_BUFF,		20000,	0,	0,	0,		0,		10000,	1.35f, "+35% dmg 10s" ),
		  AB( "Wound Refresh",	AB_AOE_HEAL,	25000,	200,	38,	0,		650,	0,	1.0f, "Full heal" ) } },

		{ "Mornan",		"Healer",	640, 32, 65, 24, 1,
		{ AB( "Balm",			AB_AOE_HEAL,		6000,	85,	16,	0,		580,	0,	1.0f, "Heal" ),
		  AB( "Hammer of Fate",		AB_DIRECT,		8500,	90,	18,	900,	0,	0,	1.0f, "Hammer" ),
		  AB( "Fortitude",			AB_BUFF,		19000,	0,	0,	0,		0,		9000,	1.3f, "+30% dmg 9s" ),
		  AB( "Healer's Hands",		AB_AOE_HEAL,	24000,	240,	45,	0,		700,	0,	1.0f, "Full heal" ) } },

		{ "Gillian",	"Assassin",	540, 26, 45, 40, 4,
		{ AB( "Shadow Stab",			AB_DIRECT,		4500,	130,	30,	1000,	0,	0,	1.0f, "Stab" ),
		  AB( "Shadow Blades",		AB_AOE_DAMAGE,	10000,	80,	18,	0,		400,	0,	1.0f, "Blade circles" ),
		  AB( "Shadow Rage",		AB_BUFF,		14000,	0,	0,	0,		0,		6000,	1.6f, "+60% dmg 6s" ),
		  AB( "Deadly Slash", AB_DIRECT,		24000,	250,	60,	1100,	0,	0,	1.0f, "Lethal strike" ) } },

		{ "Kyra",		"Assassin",	520, 24, 45, 42, 4,
		{ AB( "Backstab",		AB_DIRECT,		5000,	120,	28,	950,	0,	0,	1.0f, "Dagger" ),
		  AB( "Blood Dance",		AB_AOE_DAMAGE,	10000,	75,	16,	0,		380,	0,	1.0f, "Dance" ),
		  AB( "Hunter's Zeal",	AB_BUFF,		13000,	0,	0,	0,		0,		7000,	1.55f, "+55% dmg 7s" ),
		  AB( "Piercing Shadow",	AB_DIRECT,		25000,	260,	55,	1150,	0,	0,	1.0f, "Shadow slash" ) } },

		{ "Ravel",		"Assassin",	560, 27, 50, 38, 4,
		{ AB( "Knife Whirl",		AB_DIRECT,		4800,	110,	26,	1050,	0,	0,	1.0f, "Whirl" ),
		  AB( "Wind Blades",		AB_AOE_DAMAGE,	10000,	70,	15,	0,		420,	0,	1.0f, "Blades" ),
		  AB( "Aggression",			AB_BUFF,		15000,	0,	0,	0,		0,		8000,	1.5f, "+50% dmg 8s" ),
		  AB( "Deadly Storm",		AB_DIRECT,		23000,	230,	55,	1200,	0,	0,	1.0f, "Lethal storm" ) } },

		{ "Ismara",		"Carry",	600, 31, 65, 37, 3,
		{ AB( "Fire Volley",		AB_DIRECT,		5500,	105,	24,	1050,	0,	0,	1.0f, "Volley" ),
		  AB( "Burst Fire",	AB_AOE_DAMAGE,	10000,	70,	15,	0,		400,	0,	1.0f, "Barrage" ),
		  AB( "Warrior's Aim",		AB_BUFF,		16000,	0,	0,	0,		0,		7000,	1.45f, "+45% dmg 7s" ),
		  AB( "Finishing Shot", AB_DIRECT,		25000,	225,	50,	1200,	0,	0,	1.0f, "Harpoon shot" ) } },

		{ "Targo",		"Tank",		940, 48, 100, 30, 2,
		{ AB( "Sledgehammer",			AB_DIRECT,		6500,	100,	20,	850,	0,	0,	1.0f, "Sledgehammer" ),
		  AB( "Siege",				AB_AOE_DAMAGE,	13000,	75,	16,	0,		500,	0,	1.0f, "Siege" ),
		  AB( "Armored Assault", AB_BUFF,		21000,	0,	0,	0,		0,		10000,	1.4f, "+40% dmg 10s" ),
		  AB( "Demolition",			AB_AOE_DAMAGE,	30000,	145,	28,	0,		560,	0,	1.0f, "Demolition" ) } },

		{ "Velia",		"Mage",		530, 26, 50, 20, 2,
		{ AB( "Stardust",		AB_DIRECT,		5500,	115,	26,	1000,	0,	0,	1.0f, "Dust" ),
		  AB( "Meteor",			AB_AOE_DAMAGE,	10500,	82,	18,	0,		430,	0,	1.0f, "Meteor" ),
		  AB( "Star Rage",	AB_BUFF,		18000,	0,	0,	0,		0,		9000,	1.5f, "+50% dmg 9s" ),
		  AB( "World Fall",		AB_AOE_DAMAGE,	34000,	205,	44,	0,		600,	0,	1.0f, "New worlds" ) } },

		{ "Astarot",	"Assassin",	530, 25, 45, 44, 4,
		{ AB( "Demonic Claw", AB_DIRECT,		4500,	135,	34,	1000,	0,	0,	1.0f, "Claw" ),
		  AB( "Inferno Flame",		AB_AOE_DAMAGE,	9500,	85,	18,	0,		380,	0,	1.0f, "Flame" ),
		  AB( "Bloodthirst",		AB_BUFF,		12000,	0,	0,	0,		0,		6000,	1.65f, "+65% dmg 6s" ),
		  AB( "Death Ritual",		AB_DIRECT,		22000,	270,	65,	1100,	0,	0,	1.0f, "Lethal ritual" ) } },

		{ "Belin",		"Healer",	620, 31, 60, 22, 1,
		{ AB( "Healing Ring",	AB_AOE_HEAL,	5500,	70,	13,	0,		550,	0,	1.0f, "Ring" ),
		  AB( "Ray of Light",			AB_DIRECT,		7500,	75,	15,	1000,	0,	0,	1.0f, "Beam" ),
		  AB( "Prayer",			AB_BUFF,		20000,	0,	0,	0,		0,		10000,	1.3f, "+30% dmg 10s" ),
		  AB( "Great Miracle",		AB_AOE_HEAL,	24000,	210,	40,	0,		700,	0,	1.0f, "Miracle" ) } },

		{ "Draks",		"Tank",		960, 52, 100, 28, 2,
		{ AB( "Fire Strike",		AB_DIRECT,		6000,	95,	20,	900,	0,	0,	1.0f, "Strike" ),
		  AB( "Flame Circle",		AB_AOE_DAMAGE,	12000,	72,	15,	0,		460,	0,	1.0f, "Circle" ),
		  AB( "Dragon Rage",	AB_BUFF,		19000,	0,	0,	0,		0,		8000,	1.55f, "+55% dmg 8s" ),
		  AB( "Dragon Breath",	AB_AOE_DAMAGE,	31000,	160,	32,	0,		580,	0,	1.0f, "Flaming breath" ) } },

		{ "Elfin",		"Carry",	580, 30, 60, 40, 3,
		{ AB( "Rapid Shot",	AB_DIRECT,		5000,	95,	22,	1150,	0,	0,	1.0f, "Shot" ),
		  AB( "Arrow Lightning",		AB_DIRECT,		9000,	140,	30,	1100,	0,	0,	1.0f, "Lightning" ),
		  AB( "Swiftness",	AB_BUFF,		15000,	0,	0,	0,		0,		7000,	1.5f, "+50% dmg 7s" ),
		  AB( "Dragon Shot",	AB_DIRECT,		24000,	215,	48,	1300,	0,	0,	1.0f, "Dragon arrow" ) } },

		{ "Nomara",		"Mage",		550, 28, 55, 22, 2,
		{ AB( "Water Wave",			AB_DIRECT,		6000,	100,	22,	1000,	0,	0,	1.0f, "Wave" ),
		  AB( "Deluge",				AB_AOE_DAMAGE,	10000,	72,	16,	0,		440,	0,	1.0f, "Deluge" ),
		  AB( "Power Surge",		AB_BUFF,		17000,	0,	0,	0,		0,		8000,	1.45f, "+45% dmg 8s" ),
		  AB( "Ocean's Wrath",		AB_AOE_DAMAGE,	33000,	185,	40,	0,		570,	0,	1.0f, "Ocean" ) } },

		{ "Quinn",		"Assassin",	510, 23, 40, 45, 4,
		{ AB( "Claw Shadow",			AB_DIRECT,		4500,	140,	36,	1000,	0,	0,	1.0f, "Shadow claw" ),
		  AB( "Shadow Storm",		AB_AOE_DAMAGE,	9500,	90,	20,	0,		400,	0,	1.0f, "Storm" ),
		  AB( "Dark Grasp",		AB_BUFF,		11000,	0,	0,	0,		0,		6000,	1.7f, "+70% dmg 6s" ),
		  AB( "Rending Claw", AB_DIRECT,		21000,	280,	70,	1100,	0,	0,	1.0f, "Burst" ) } },

		{ "Charon",		"Tank",		970, 54, 100, 26, 2,
		{ AB( "Bone Strike",		AB_DIRECT,		6500,	88,	17,	850,	0,	0,	1.0f, "Strike" ),
		  AB( "Bone Wall",		AB_AOE_DAMAGE,	13000,	70,	14,	0,		470,	0,	1.0f, "Wall" ),
		  AB( "Cold Wrath",		AB_BUFF,		20000,	0,	0,	0,		0,		9000,	1.5f, "+50% dmg 9s" ),
		  AB( "Death and Bones",		AB_AOE_DAMAGE,	29000,	150,	30,	0,		540,	0,	1.0f, "Bone storm" ) } },

		{ "Ignis",		"Mage",		520, 25, 50, 22, 2,
		{ AB( "Spark",				AB_DIRECT,		5000,	120,	28,	1000,	0,	0,	1.0f, "Spark" ),
		  AB( "Arson",				AB_AOE_DAMAGE,	9500,	85,	18,	0,		420,	0,	1.0f, "Arson" ),
		  AB( "Burning Blood",		AB_BUFF,		15000,	0,	0,	0,		0,		7000,	1.5f, "+50% dmg 7s" ),
		  AB( "Burning World",		AB_AOE_DAMAGE,	32000,	200,	42,	0,		600,	0,	1.0f, "World of fire" ) } },

		{ "Valka",		"Healer",	630, 33, 65, 22, 1,
		{ AB( "Healing Dew",		AB_AOE_HEAL,	5000,	70,	14,	0,		600,	0,	1.0f, "Dew" ),
		  AB( "Ray of Dawn",			AB_DIRECT,		8000,	85,	17,	1000,	0,	0,	1.0f, "Beam" ),
		  AB( "Dawn Charge",		AB_BUFF,		18000,	0,	0,	0,		0,		10000,	1.4f, "+40% dmg 10s" ),
		  AB( "Daybreak",			AB_AOE_HEAL,		23000,	230,	42,	0,		700,	0,	1.0f, "Greater heal" ) } } };

	int i;

	for ( i = 0; i < MOBA_MAX_HEROES; i++ )
	{
		mobaHeroes[i] = heroes[i];
	}

	mobaNumHeroes = MOBA_MAX_HEROES;
}