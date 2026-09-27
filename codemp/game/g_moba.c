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
static void MOBA_StartDraft( void );
static void MOBA_StartAssign( void );
static void MOBA_DraftPrompt( void );
static void MOBA_TransferCaptain( int seat, const char *why );
static void MOBA_RunAutoDraft( void );
static void MOBA_ApplyHeroModel( gentity_t *ent );

#define MOBA_DEFAULT_MAXHEALTH	500
#define MOBA_DEFAULT_ARMOR		50

// JKA armor is a shield pool that depletes as it eats damage (G_ApplyArmor),
// not a mitigation percentage, so this is a shield point budget. It has to stay
// above the strongest hero base armor (100) or the armor items are dead gold
// for exactly the heroes that are supposed to buy them.
#define MOBA_MAX_ARMOR			200
#define MOBA_MAX_HEALTH			10000

// Two players that spawn closer than this are treated as standing on the same
// spot: the spawn logic only uses a spot when it is this far away from every
// other living player.
#define MOBA_SPAWN_CLEAR		192.0f

// gold and xp for a hero kill, the round win and round loss payouts are in
// MOBA_StartRoundEnd
#define MOBA_GOLD_KILL			300
#define MOBA_XP_KILL_BASE		50
#define MOBA_XP_KILL_PER_LEVEL	10

#define MOBA_FALLBACK_RESPAWN	3600000	// 1h - effectively never (round-based respawn)

typedef enum {
	MOBA_DRAFT_BAN = 0,
	MOBA_DRAFT_PICK
} mobaDraftAction_t;

// The captain array and the plan are indexed by seat, not by team: this engine
// numbers TEAM_FREE 0, TEAM_RED 1 and TEAM_BLUE 2, so a team_t can never be used
// as an index.
#define MOBA_DRAFT_SEATS	2

static qboolean mobaEnabled = qfalse;
static mobaPhase_t mobaPhase = MOBA_PHASE_LOBBY;
static int mobaPhaseEnd = 0;
static int mobaRound = 0;
static int mobaRedAlive = 0, mobaBlueAlive = 0;
static int mobaRedPlayers = 0, mobaBluePlayers = 0;

mobaPlayer_t mobaPlayers[MAX_CLIENTS];

// ---- Draft state, the draft runs once per match ----
// The plan is a flat list of (action, seat) pairs that is built from the roster
// at draft start, so the walk through it needs no per captain counters.
static qboolean mobaHeroBanned[MOBA_MAX_HEROES];
static team_t mobaHeroTeam[MOBA_MAX_HEROES];		// TEAM_FREE = still up for grabs

static qboolean mobaDraftDone;					// a draft finished in this match
static int mobaCaptain[MOBA_DRAFT_SEATS];		// clientNum of the captain per seat
static mobaDraftAction_t mobaDraftPlan[MOBA_DRAFT_ACTIONS_MAX];
static int mobaDraftActor[MOBA_DRAFT_ACTIONS_MAX];
static int mobaDraftPlanLen = 0;
static int mobaDraftStep = 0;					// index of the step to resolve
static int mobaDraftFirst = 0;					// seat that opens every stage
static int mobaAutoDraftNext = 0;				// level.time of the next auto step

static team_t MOBA_SeatTeam( int seat )
{
	return ( seat == 0 ) ? TEAM_RED : TEAM_BLUE;
}

static int MOBA_TeamSeat( team_t team )
{
	return ( team == TEAM_RED ) ? 0 : 1;
}

//=========================================================================
// Hero / item tables
//=========================================================================

mobaHero_t mobaHeroes[MOBA_MAX_HEROES];
int mobaNumHeroes = 0;

// The category is the group the client shows as a tab, and it has to match
// cgMobaItemCats in cg_moba.c. The order of this table is the order of the item
// mask, so the two tables may never be sorted apart.
mobaItem_t mobaItems[] = {
	{ "Sturdy Armor",	"armor",	"DEFENCE",		250,	50,	0,	0,	"+50 armor" },
	{ "Med Kit",		"med",		"CONSUMABLES",	200,	0,	100, 0,	"+100 health" },
	{ "Rage Rune",		"rage",		"ATTACK",	300,	0,	0,	20,	"+20% damage" },
	{ "Heavy Plate",	"heavy",	"DEFENCE",		500,	100, 50, 0,	"+100 armor, +50 health" },
	{ "Power Crystal",	"crystal",	"ATTACK",	650,	0,	50,	40,	"+50 health, +40% damage" },
	{ "Shadow Cloak",	"cloak",	"DEFENCE",		400,	30,	0,	15,	"+30 armor, +15% damage" }
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
// Captain draft - one ban/pick plan per match
//=========================================================================

static qboolean MOBA_SlotActive( int clientNum )
{
	gentity_t *ent;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS )
	{
		return qfalse;
	}

	ent = &g_entities[clientNum];

	return ( ent->inuse && ent->client &&
		ent->client->pers.connected == CON_CONNECTED ) ? qtrue : qfalse;
}

static int MOBA_TeamSize( team_t team )
{
	int i, count = 0;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( MOBA_SlotActive( i ) && g_entities[i].client->sess.sessionTeam == team )
		{
			count++;
		}
	}

	return count;
}

static void MOBA_ResetHeroPool( void )
{
	int i;

	for ( i = 0; i < MOBA_MAX_HEROES; i++ )
	{
		mobaHeroBanned[i] = qfalse;
		mobaHeroTeam[i] = TEAM_FREE;
	}

	mobaDraftPlanLen = 0;
	mobaDraftStep = 0;
	mobaCaptain[0] = -1;
	mobaCaptain[1] = -1;
}

// One captain per team, drawn at random. With the autodraft aid on a human is
// preferred, so the draft can be driven from a real client while the bots would
// eat both captain jobs.
static int MOBA_ElectCaptain( team_t team )
{
	int i, humans[MAX_CLIENTS], anyone[MAX_CLIENTS], numHumans = 0, numAny = 0;
	int *pool, num;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( !MOBA_SlotActive( i ) || g_entities[i].client->sess.sessionTeam != team )
		{
			continue;
		}

		anyone[numAny++] = i;
		if ( !( g_entities[i].r.svFlags & SVF_BOT ) )
		{
			humans[numHumans++] = i;
		}
	}

	pool = ( moba_autodraft.integer && numHumans > 0 ) ? humans : anyone;
	num = ( moba_autodraft.integer && numHumans > 0 ) ? numHumans : numAny;

	if ( num <= 0 )
	{
		return -1;
	}

	return pool[Q_irand( 0, num - 1 )];
}

// A captain that goes quiet or leaves hands the job to another player of the
// team. A team alone keeps the job, the step then simply times out.
static void MOBA_TransferCaptain( int seat, const char *why )
{
	int i, start, guard, candidates[MAX_CLIENTS], count = 0;
	int old = mobaCaptain[seat];
	team_t team = MOBA_SeatTeam( seat );
	gentity_t *ent;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( i == old || !MOBA_SlotActive( i ) )
		{
			continue;
		}
		if ( g_entities[i].client->sess.sessionTeam != team )
		{
			continue;
		}

		candidates[count++] = i;
	}

	if ( count <= 0 )
	{
		return;
	}

	start = Q_irand( 0, count - 1 );
	mobaCaptain[seat] = candidates[start];
	ent = &g_entities[mobaCaptain[seat]];

	MOBA_CPAll( "^3Captaincy of the %s team passes to %s (%s)\n",
		team == TEAM_RED ? "^1red^7" : "^1blue^7", ent->client->pers.netname, why );
	MOBA_CPSelf( ent, "You are the ^3captain^7 now.\n" );
}

// Builds the flat step list from the current roster: three stages of two bans
// per captain, each followed by two picks per captain. Whatever is still
// unpicked after the second stage is taken in the third one, so a 5v5 ends on a
// single pick per captain. The number of bans shrinks if the roster is too big
// for the hero pool.
static void MOBA_BuildDraftPlan( void )
{
	int stage, i, seat, bans[MOBA_DRAFT_SEATS], picks[MOBA_DRAFT_SEATS];
	int banBudget, totalPicks;

	totalPicks = MOBA_TeamSize( TEAM_RED ) + MOBA_TeamSize( TEAM_BLUE );

	// every pick needs a hero of its own, whatever is left pays for the bans
	banBudget = ( mobaNumHeroes - totalPicks ) / 2;
	if ( banBudget > MOBA_DRAFT_STAGES * MOBA_DRAFT_BANS_PER_CAPTAIN )
	{
		banBudget = MOBA_DRAFT_STAGES * MOBA_DRAFT_BANS_PER_CAPTAIN;
	}
	if ( banBudget < 0 )
	{
		banBudget = 0;
	}

	for ( seat = 0; seat < MOBA_DRAFT_SEATS; seat++ )
	{
		bans[seat] = banBudget;
		// a captain picks as many heroes as his own team has players
		picks[seat] = MOBA_TeamSize( MOBA_SeatTeam( seat ) );
	}

	mobaDraftFirst = Q_irand( 0, MOBA_DRAFT_SEATS - 1 );
	mobaDraftPlanLen = 0;

	for ( stage = 0; stage < MOBA_DRAFT_STAGES; stage++ )
	{
		// ban part: MOBA_DRAFT_BANS_PER_CAPTAIN per captain, one hero per turn
		for ( i = 0; i < MOBA_DRAFT_BANS_PER_CAPTAIN * 2; i++ )
		{
			seat = ( mobaDraftFirst + i ) % MOBA_DRAFT_SEATS;
			if ( bans[seat] <= 0 || mobaDraftPlanLen >= MOBA_DRAFT_ACTIONS_MAX )
			{
				continue;
			}

			bans[seat]--;
			mobaDraftPlan[mobaDraftPlanLen] = MOBA_DRAFT_BAN;
			mobaDraftActor[mobaDraftPlanLen] = seat;
			mobaDraftPlanLen++;
		}

		// pick part: MOBA_DRAFT_PICKS_PER_STAGE per captain while both still
		// have picks left, the last stage takes the remainder
		for ( i = 0; i < MOBA_DRAFT_PICKS_PER_STAGE * 2; i++ )
		{
			seat = ( mobaDraftFirst + i ) % MOBA_DRAFT_SEATS;
			if ( picks[seat] <= 0 || mobaDraftPlanLen >= MOBA_DRAFT_ACTIONS_MAX )
			{
				continue;
			}

			picks[seat]--;
			mobaDraftPlan[mobaDraftPlanLen] = MOBA_DRAFT_PICK;
			mobaDraftActor[mobaDraftPlanLen] = seat;
			mobaDraftPlanLen++;
		}
	}
}

// A hero that is neither banned nor picked yet, picked at random.
static int MOBA_DraftFreeHero( void )
{
	int i, start, guard;

	if ( mobaNumHeroes <= 0 )
	{
		return -1;
	}

	start = Q_irand( 0, mobaNumHeroes - 1 );
	for ( guard = 0; guard < mobaNumHeroes; guard++ )
	{
		i = ( start + guard ) % mobaNumHeroes;
		if ( !mobaHeroBanned[i] && mobaHeroTeam[i] == TEAM_FREE )
		{
			return i;
		}
	}

	return -1;
}

// The heroes a team picked in the captain stage, in pick order. Returns the
// count and fills the array, the pool can hold every hero of the match.
static int MOBA_TeamPool( team_t team, int *heroIds, int maxIds )
{
	int i, count = 0;

	for ( i = 0; i < mobaNumHeroes && count < maxIds; i++ )
	{
		if ( mobaHeroTeam[i] == team )
		{
			heroIds[count++] = i;
		}
	}

	return count;
}

static qboolean MOBA_IsActingCaptain( gentity_t *ent )
{
	if ( !ent || !ent->client || mobaDraftStep >= mobaDraftPlanLen )
	{
		return qfalse;
	}

	return ( mobaCaptain[mobaDraftActor[mobaDraftStep]] == ent->s.number ) ? qtrue : qfalse;
}

static void MOBA_DraftPrompt( void )
{
	int seat, captain, secs;
	team_t team;
	gentity_t *ent;
	qboolean ban;

	if ( mobaDraftStep >= mobaDraftPlanLen )
	{
		return;
	}

	seat = mobaDraftActor[mobaDraftStep];
	team = MOBA_SeatTeam( seat );
	captain = mobaCaptain[seat];
	ban = ( mobaDraftPlan[mobaDraftStep] == MOBA_DRAFT_BAN ) ? qtrue : qfalse;
	secs = ( mobaPhaseEnd - level.time ) / 1000;

	MOBA_CPAll( "^3Draft %i/%i^7 - the %s team has to ^3%s^7 a hero (%i s)\n",
		mobaDraftStep + 1, mobaDraftPlanLen,
		team == TEAM_RED ? "^1red^7" : "^1blue^7", ban ? "ban" : "pick", secs );

	if ( !MOBA_SlotActive( captain ) )
	{
		// nobody is left to act, the step resolves itself on the clock
		MOBA_TransferCaptain( seat, "no captain" );
		return;
	}

	ent = &g_entities[captain];
	MOBA_CPSelf( ent, "You are the ^3captain^7! Type ^3!%s N^7, !heroes lists the pool (%i s)\n",
		ban ? "ban" : "pick", secs );
}

// Resolves the current step: with a hero the captain named, or with a random
// one when heroId is negative. A negative id also covers the timeout and the
// autodraft aid, which cannot fail on an empty pool and just skip the step.
static qboolean MOBA_DraftCommit( int heroId, const char *how )
{
	int seat, captain;
	team_t team;
	qboolean ban, blind = ( heroId < 0 ) ? qtrue : qfalse;
	const char *capName;

	if ( mobaDraftStep >= mobaDraftPlanLen )
	{
		return qfalse;
	}

	seat = mobaDraftActor[mobaDraftStep];
	team = MOBA_SeatTeam( seat );
	captain = mobaCaptain[seat];
	ban = ( mobaDraftPlan[mobaDraftStep] == MOBA_DRAFT_BAN ) ? qtrue : qfalse;

	if ( blind )
	{
		heroId = MOBA_DraftFreeHero();
	}
	else if ( heroId < 0 || heroId >= mobaNumHeroes || mobaHeroBanned[heroId] ||
		mobaHeroTeam[heroId] != TEAM_FREE )
	{
		return qfalse;
	}

	if ( heroId >= 0 )
	{
		if ( ban )
		{
			mobaHeroBanned[heroId] = qtrue;
		}
		else
		{
			mobaHeroTeam[heroId] = team;
		}

		capName = MOBA_SlotActive( captain ) ?
			g_entities[captain].client->pers.netname : "the team";

		if ( ban )
		{
			MOBA_CPAll( "^1BAN^7 - %s (%s) bans ^5%s^7\n", capName, how,
				mobaHeroes[heroId].name );
		}
		else
		{
			MOBA_CPAll( "^2PICK^7 - %s (%s) takes ^5%s^7 for the %s team\n", capName, how,
				mobaHeroes[heroId].name, team == TEAM_RED ? "^1red^7" : "^1blue^7" );
		}
	}
	else
	{
		MOBA_CPAll( "^3Draft step skipped, no hero left (%s)\n", how );
	}

	mobaDraftStep++;
	mobaPhaseEnd = level.time + moba_pickTime.integer * 1000;

	if ( mobaDraftStep >= mobaDraftPlanLen )
	{
		MOBA_StartAssign();
	}
	else
	{
		MOBA_DraftPrompt();
	}

	return qtrue;
}

//=========================================================================
// Hero assignment
//=========================================================================

static qboolean MOBA_HeroTaken( int heroId )
{
	int i;

	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		// the connection, not the spawn, owns the hero: a client that has not
		// spawned yet still blocks the hero it was given
		if ( MOBA_SlotActive( i ) && mobaPlayers[i].heroId == heroId )
		{
			return qtrue;
		}
	}

	return qfalse;
}

// Hands a hero to every player that has none: a player who sat out the assign
// stage gets one out of the pool of his own team first and any free hero only if
// that pool is used up. Must run before the respawn of the buy phase, the stats
// are only applied on spawn.
static void MOBA_FillMissingHeroes( void )
{
	int i, heroId, guard, pool[MOBA_MAX_HEROES], poolCount, j;
	team_t team;

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *ent = &g_entities[i];

		if ( !MOBA_SlotActive( i ) || mobaPlayers[i].heroId >= 0 )
		{
			continue;
		}

		team = ent->client->sess.sessionTeam;
		heroId = -1;

		// own team pool first, the captains picked exactly one hero per player
		if ( team == TEAM_RED || team == TEAM_BLUE )
		{
			poolCount = MOBA_TeamPool( team, pool, ARRAY_LEN( pool ) );
			for ( j = 0; j < poolCount; j++ )
			{
				if ( !MOBA_HeroTaken( pool[j] ) )
				{
					heroId = pool[j];
					break;
				}
			}
		}

		// fall back to any free hero, a team that grew after the draft has no
		// pool left for its late joiners
		if ( heroId < 0 )
		{
			// start random and walk on until a free hero shows up, with fewer
			// clients than heroes the walk always ends before the list runs out
			heroId = Q_irand( 0, mobaNumHeroes - 1 );
			for ( guard = 0; guard < mobaNumHeroes &&
				( mobaHeroBanned[heroId] || MOBA_HeroTaken( heroId ) ); guard++ )
			{
				heroId = ( heroId + 1 ) % mobaNumHeroes;
			}
		}

		mobaPlayers[i].heroId = heroId;
		MOBA_ApplyHeroModel( ent );
		MOBA_LogLine( va( "%s plays %s", ent->client->pers.netname,
			mobaHeroes[heroId].name ), ent );
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

// The draft and the round logic both need a player on each side. Balancing on
// connect can still leave everybody on one team, so the last player of the full
// team is moved over instead of waiting for a team that will never fill itself.
static void MOBA_BalanceTeams( void )
{
	team_t from, to;
	int i;

	if ( MOBA_TeamHasPlayer( TEAM_RED ) && MOBA_TeamHasPlayer( TEAM_BLUE ) )
	{
		return;
	}

	if ( MOBA_TeamHasPlayer( TEAM_RED ) )
	{
		from = TEAM_RED;
		to = TEAM_BLUE;
	}
	else
	{
		from = TEAM_BLUE;
		to = TEAM_RED;
	}

	// a lone player is not moved anywhere, the lobby waits for a second one
	if ( MOBA_TeamSize( from ) < 2 )
	{
		return;
	}

	for ( i = level.maxclients - 1; i >= 0; i-- )
	{
		gentity_t *ent = &g_entities[i];

		if ( MOBA_SlotActive( i ) && ent->client->sess.sessionTeam == from )
		{
			SetTeamQuick( ent, to, qfalse );
			MOBA_LogLine( va( "%s moved to the %s team to fill it up",
				ent->client->pers.netname, to == TEAM_RED ? "red" : "blue" ), ent );
			return;
		}
	}
}

// True when no connected player is left without a hero, which ends the assign
// stage before the clock runs out.
static qboolean MOBA_EveryoneHasHero( void )
{
	int i;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( MOBA_SlotActive( i ) && mobaPlayers[i].heroId < 0 )
		{
			return qfalse;
		}
	}

	return qtrue;
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
	MOBA_CPAll( "^2Magic Wands^7 - waiting for players on both teams (2+)\n" );
}

static void MOBA_StartDraft( void )
{
	mobaPhase = MOBA_PHASE_DRAFT;

	MOBA_EnsureTeams();
	MOBA_BalanceTeams();

	// a single player has nobody to draft against, the lobby keeps waiting
	if ( !MOBA_TeamHasPlayer( TEAM_RED ) || !MOBA_TeamHasPlayer( TEAM_BLUE ) )
	{
		mobaPhase = MOBA_PHASE_LOBBY;
		return;
	}

	// a draft that was interrupted before it finished starts over, the heroes
	// of the previous attempt are meaningless
	MOBA_ResetHeroPool();

	mobaCaptain[0] = MOBA_ElectCaptain( TEAM_RED );
	mobaCaptain[1] = MOBA_ElectCaptain( TEAM_BLUE );

	MOBA_BuildDraftPlan();

	if ( mobaDraftPlanLen <= 0 )
	{
		MOBA_StartAssign();
		return;
	}

	mobaPhaseEnd = level.time + moba_pickTime.integer * 1000;

	MOBA_CPAll( "^3Hero draft!^7 %i steps, %i sec each - the captains ban and pick\n",
		mobaDraftPlanLen, moba_pickTime.integer );
	MOBA_DraftPrompt();
}

// Every player of a team takes one hero out of the pool his captain picked. The
// stage ends as soon as nobody is left without a hero, so a quick team does not
// wait out the clock.
static void MOBA_StartAssign( void )
{
	int i, missing = 0;

	// the lineup of the match is fixed from here on, the later rounds reuse it
	mobaDraftDone = qtrue;

	mobaPhase = MOBA_PHASE_DRAFT_ASSIGN;
	mobaPhaseEnd = level.time + moba_pickTime.integer * 1000;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( MOBA_SlotActive( i ) && mobaPlayers[i].heroId < 0 )
		{
			missing++;
		}
	}

	MOBA_CPAll( "^3Hero assign!^7 %i player(s) still need a hero: ^3!pick N^7 - "
		"!heroes lists the pool of your team (%i s)\n", missing, moba_pickTime.integer );
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
	MOBA_ResetHeroPool();

	mobaDraftDone = qfalse;
	mobaDraftFirst = 0;
	mobaAutoDraftNext = 0;
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

// Resolves every draft step with a random hero instead of waiting out the clock,
// so a full ban/pick plan can be verified on a dedicated server. It also makes
// the captain election prefer a human, which is what allows the draft to be
// driven by hand while the bots hold the other captain jobs.
static void MOBA_RunAutoDraft( void )
{
	if ( !moba_autodraft.integer || mobaPhase != MOBA_PHASE_DRAFT ||
		mobaDraftStep >= mobaDraftPlanLen || level.time < mobaAutoDraftNext )
	{
		return;
	}

	mobaAutoDraftNext = level.time + moba_autodraftEvery.integer;
	MOBA_DraftCommit( -1, "auto" );
}

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

	// '!' commands are spread over the draft, the assign stage, the buy and the
	// fight phase
	if ( mobaPhase != MOBA_PHASE_DRAFT && mobaPhase != MOBA_PHASE_DRAFT_ASSIGN &&
		mobaPhase != MOBA_PHASE_BUY && mobaPhase != MOBA_PHASE_FIGHT )
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
static char mobaLastDraftSent[MAX_CLIENTS][128];
static char mobaAbilitiesSent[MAX_CLIENTS][64];
static int mobaAbilitiesNext[MAX_CLIENTS];
static qboolean mobaShopStateLogged[MAX_CLIENTS];
static qboolean mobaDraftStateLogged[MAX_CLIENTS];

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

//=========================================================================
// Pushes the draft picture to one client so the hero select window can render
// the bans, the team picks and the turn of that very client:
// "mobaDraft banned red blue action canAct seconds myHero step steps".
//
// The three hero sets are sent as bit masks: 30 heroes fit into an int, one
// command carries the whole state and the cgame never has to ask the server
// twice for the same thing. action is 0 nothing to do, 1 ban, 2 pick and
// canAct is the only field the client is not allowed to guess, because "whose
// turn is it" changes whenever a captain times out or disconnects.
//=========================================================================
static void MOBA_PushDraftState( int clientNum )
{
	gentity_t *ent = &g_entities[clientNum];
	char buf[128];
	int banned = 0, red = 0, blue = 0, taken = 0, i, action, canAct, secs, myTeam;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS ||
		!ent->inuse || !ent->client ||
		ent->client->pers.connected != CON_CONNECTED || !mobaPlayers[clientNum].inuse )
	{
		return;
	}

	if ( mobaPhase != MOBA_PHASE_DRAFT && mobaPhase != MOBA_PHASE_DRAFT_ASSIGN )
	{
		// outside the draft the window is closed, but the last masks have to be
		// cleared so a client that missed a phase change cannot draw a stale
		// board on the next draft
		if ( mobaLastDraftSent[clientNum][0] != '\0' )
		{
			mobaLastDraftSent[clientNum][0] = '\0';
			trap->SendServerCommand( ent->s.number,
				"mobaDraft \"0 0 0 0 0 0 0 0 -1 0 0\"" );
		}
		return;
	}

	for ( i = 0; i < mobaNumHeroes && i < MOBA_MAX_HEROES; i++ )
	{
		if ( mobaHeroBanned[i] )
		{
			banned |= ( 1 << i );
		}
		else if ( mobaHeroTeam[i] == TEAM_RED )
		{
			red |= ( 1 << i );
		}
		else if ( mobaHeroTeam[i] == TEAM_BLUE )
		{
			blue |= ( 1 << i );
		}
	}

	// the pool masks only say which heroes a team may hand out, not which of them
	// a player already took. Without this the window cannot tell a free hero from
	// a taken one and the confirm button would offer a hero the server refuses.
	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		if ( mobaPlayers[i].inuse && mobaPlayers[i].heroId >= 0 && mobaPlayers[i].heroId < MOBA_MAX_HEROES )
		{
			taken |= ( 1 << mobaPlayers[i].heroId );
		}
	}

	action = 0;
	canAct = 0;

	if ( mobaPhase == MOBA_PHASE_DRAFT )
	{
		// the plan can be walked out one frame before the phase switches to the
		// assign stage, and mobaDraftPlan is a fixed size array
		if ( mobaDraftStep < mobaDraftPlanLen )
		{
			action = ( mobaDraftPlan[mobaDraftStep] == MOBA_DRAFT_BAN ) ? 1 : 2;
		}

		canAct = MOBA_IsActingCaptain( ent ) ? 1 : 0;
	}
	else
	{
		// every player takes one hero out of the pool of his own team, so the
		// action is always a pick here
		action = 2;
		canAct = ( mobaPlayers[clientNum].heroId < 0 &&
			( ent->client->sess.sessionTeam == TEAM_RED ||
				ent->client->sess.sessionTeam == TEAM_BLUE ) ) ? 1 : 0;
	}

	secs = ( mobaPhaseEnd > level.time ) ? ( mobaPhaseEnd - level.time + 999 ) / 1000 : 0;
	myTeam = ent->client->sess.sessionTeam;

	Com_sprintf( buf, sizeof( buf ), "%i %i %i %i %i %i %i %i %i %i %i",
		banned, red, blue, taken, action, canAct, secs,
		mobaPlayers[clientNum].heroId, myTeam, mobaDraftStep, mobaDraftPlanLen );

	if ( Q_stricmp( buf, mobaLastDraftSent[clientNum] ) != 0 )
	{
		Q_strncpyz( mobaLastDraftSent[clientNum], buf, sizeof( mobaLastDraftSent[clientNum] ) );
		trap->SendServerCommand( ent->s.number, va( "mobaDraft \"%s\"", buf ) );

		if ( !mobaDraftStateLogged[clientNum] )
		{
			mobaDraftStateLogged[clientNum] = qtrue;
			MOBA_LogLine( va( "draft state -> client %i: %s", clientNum, buf ), NULL );
		}
	}
}

//=========================================================================
// Pushes the four ability slots of one client to its own cgame:
// "mobaAbilities heroId cd0 cd1 cd2 cd3 lv0 lv1 lv2 lv3".
//
// The hero id is repeated even though the draft push already carries it,
// because the bar in the fight phase has to know which hero it draws and the
// hero is not part of the shop state. cd is the milliseconds a slot still has
// to wait, lv the rank the player bought, so the client can grey the slot out
// and count down without asking anything.
//
// A running cooldown changes the numbers on every frame, so the push is rate
// limited to five times a second; the client counts the remaining time down on
// its own between the pushes, exactly like the phase timer.
//=========================================================================
static void MOBA_PushAbilityState( int clientNum )
{
	gentity_t *ent = &g_entities[clientNum];
	mobaPlayer_t *p = &mobaPlayers[clientNum];
	char buf[64];
	int i, cd[MOBA_ABILITIES_PER_HERO], lv[MOBA_ABILITIES_PER_HERO];

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS ||
		!ent->inuse || !ent->client ||
		ent->client->pers.connected != CON_CONNECTED || !p->inuse )
	{
		return;
	}

	if ( p->heroId < 0 || p->heroId >= mobaNumHeroes )
	{
		// no hero yet, so a bar of a previous hero must not stay on screen
		if ( mobaAbilitiesSent[clientNum][0] != '\0' )
		{
			mobaAbilitiesSent[clientNum][0] = '\0';
			trap->SendServerCommand( ent->s.number, "mobaAbilities \"-1 0 0 0 0 0 0 0 0\"" );
		}
		return;
	}

	for ( i = 0; i < MOBA_ABILITIES_PER_HERO; i++ )
	{
		cd[i] = ( p->cdReady[i] > level.time ) ? ( p->cdReady[i] - level.time ) : 0;
		lv[i] = p->abilityLevel[i];
	}

	Com_sprintf( buf, sizeof( buf ), "%i %i %i %i %i %i %i %i %i", p->heroId,
		cd[0], cd[1], cd[2], cd[3], lv[0], lv[1], lv[2], lv[3] );

	if ( Q_stricmp( buf, mobaAbilitiesSent[clientNum] ) == 0 )
	{
		return;
	}

	// the very first push of a slot goes out right away, a later change waits for
	// the tick of the rate limit
	if ( mobaAbilitiesSent[clientNum][0] != '\0' && level.time < mobaAbilitiesNext[clientNum] )
	{
		return;
	}

	mobaAbilitiesNext[clientNum] = level.time + 200;
	Q_strncpyz( mobaAbilitiesSent[clientNum], buf, sizeof( mobaAbilitiesSent[clientNum] ) );
	trap->SendServerCommand( ent->s.number, va( "mobaAbilities \"%s\"", buf ) );
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
	MOBA_RunAutoDraft();
	MOBA_RunTestKill();
	MOBA_RunTestKick();

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		MOBA_PushShopState( i );
		MOBA_PushDraftState( i );
		MOBA_PushAbilityState( i );
	}

	switch ( mobaPhase )
	{
	case MOBA_PHASE_LOBBY:
		if ( level.numConnectedClients >= 2 )
		{
			// the draft runs once per match, every later round goes straight to
			// the shop with the lineup of the first one
			if ( mobaDraftDone )
			{
				MOBA_StartBuy();
			}
			else
			{
				MOBA_StartDraft();
			}
		}
		break;

	case MOBA_PHASE_DRAFT:
		// a client that joins in the middle of the draft still has to end up on
		// a team, it is too late for its captain job but not for the match
		MOBA_EnsureTeams();
		if ( level.time >= mobaPhaseEnd )
		{
			if ( mobaDraftStep >= mobaDraftPlanLen )
			{
				MOBA_StartAssign();
			}
			else
			{
				// a captain that did not act in time costs the step a random
				// hero and the job goes to somebody else of the team
				MOBA_TransferCaptain( mobaDraftActor[mobaDraftStep], "no answer" );
				MOBA_DraftCommit( -1, "timeout" );
			}
		}
		break;

	case MOBA_PHASE_DRAFT_ASSIGN:
		MOBA_EnsureTeams();
		if ( level.time >= mobaPhaseEnd || MOBA_EveryoneHasHero() )
		{
			MOBA_StartBuy();
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
				// no re-draft: the lineup of the match stays, the round waits
				// for players to show up
				MOBA_StartLobby();
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
				MOBA_StartLobby();
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
				MOBA_StartLobby();
			}
		}
		break;
	}
}

qboolean MOBA_Active( void )
{
	return mobaEnabled;
}

mobaPhase_t MOBA_GetPhase( void )
{
	return mobaPhase;
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

//=========================================================================
// Hero model
//=========================================================================
// A player walks into the match as the hero he picked, so every hero carries
// the name of a stock models/players model. The client renders whatever model
// its own userinfo names, and the server is the only side that can change it,
// so the hero model is written into the userinfo and the clientinfo is rebuilt:
// every other client picks the new body up through CS_PLAYERS on the next
// snapshot.
//
// The team suffix is not decoration. A team game validates the skin against the
// model and falls back to model_red.skin or model_blue.skin, and every model in
// the table ships both, so the teams stay apart visually.
//
// This is not a stock client feature: it needs d_perPlayerGhoul2 1 from
// moba.cfg, otherwise the server keeps one shared Kyle body for hit detection
// while the clients draw something else.
static void MOBA_ApplyHeroModel( gentity_t *ent )
{
	mobaPlayer_t *p;
	char userinfo[MAX_INFO_STRING] = {0}, want[MAX_QPATH];
	const char *skin, *current;
	int num;

	if ( !mobaEnabled || !ent->client )
	{
		return;
	}

	num = ent->s.number;
	if ( num < 0 || num >= MAX_CLIENTS )
	{
		return;
	}

	p = &mobaPlayers[num];

	if ( !p->inuse ||
		ent->client->pers.connected != CON_CONNECTED ||
		p->heroId < 0 || p->heroId >= mobaNumHeroes ||
		!mobaHeroes[p->heroId].model || !mobaHeroes[p->heroId].model[0] )
	{
		return;
	}

	skin = ( ent->client->sess.sessionTeam == TEAM_BLUE ) ? "blue" : "red";
	Q_strncpyz( want, va( "%s/%s", mobaHeroes[p->heroId].model, skin ), sizeof( want ) );

	trap->GetUserinfo( num, userinfo, sizeof( userinfo ) );
	current = Info_ValueForKey( userinfo, "model" );

	// The client sends its own userinfo on every respawn and would put the model
	// cvar back, so this has to run on every spawn. It only writes when the
	// value really differs, because ClientUserinfoChanged rebuilds the whole
	// clientinfo string and re-registers the skin.
	if ( current && !Q_stricmp( current, want ) )
	{
		return;
	}

	Info_SetValueForKey( userinfo, "model", want );
	trap->SetUserinfo( num, userinfo );
	ClientUserinfoChanged( num );
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
	MOBA_ApplyHeroModel( ent );

	// A player who never saw the manual has no way to find the commands, so the
	// hint goes out once per connection instead of waiting for !help.
	if ( !mobaPlayers[ent->s.number].greeted )
	{
		mobaPlayers[ent->s.number].greeted = qtrue;
		MOBA_CPSelf( ent, "^3Magic Wands^7: !heroes - hero list,\n"
			"^3!pick N^7 - hero, ^3!ban N^7 - draft ban,\n"
			"^3B^7 - open or close the shop window in the buy phase,\n"
			"^3ESC^7 closes the shop, a left click buys the item under it,\n"
			"!buy N|code - buy, !buyall - buy everything affordable,\n"
			"!upgrade N - upgrade ability, ^3Q E C V^7 - abilities on the bar,\n"
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
		team_t team = ent->client->sess.sessionTeam;

		// a captain that leaves mid draft hands the job over, otherwise the
		// team would lose its turn and eat a random hero on the clock
		if ( ( team == TEAM_RED || team == TEAM_BLUE ) &&
			mobaCaptain[MOBA_TeamSeat( team )] == ent->s.number &&
			mobaPhase == MOBA_PHASE_DRAFT )
		{
			MOBA_TransferCaptain( MOBA_TeamSeat( team ), "captain left" );
		}

		memset( &mobaPlayers[ent->s.number], 0, sizeof( mobaPlayers[ent->s.number] ) );
		mobaPlayers[ent->s.number].heroId = -1;
		mobaPlayers[ent->s.number].autoCmdNext = 0;
		mobaPlayers[ent->s.number].autoCmdIdx = 0;
		mobaLastSent[ent->s.number][0] = '\0';
		mobaShopStateLogged[ent->s.number] = qfalse;
		mobaLastDraftSent[ent->s.number][0] = '\0';
		mobaDraftStateLogged[ent->s.number] = qfalse;
		mobaAbilitiesSent[ent->s.number][0] = '\0';
		mobaAbilitiesNext[ent->s.number] = 0;
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
		int i, j, best = 0, clear = 0;
		int clearSpots[ARRAY_LEN( ents )];
		float gap[ARRAY_LEN( ents )], bestGap = -1.0f;

		// Picking a spot at random drops two players onto the same origin, and two
		// bodies in one spot means one of them is stuck inside the other. So every
		// spot is scored by how far the closest living player stands from it.
		//
		// Spots that are far enough from everybody are candidates and one of them
		// is taken at random, which keeps the spawns varied. Only when the map has
		// fewer spots than players does the emptiest one win, so a full spawn
		// area still puts everybody as far apart as it can.
		for ( i = 0; i < count; i++ )
		{
			gap[i] = 999999.0f;

			for ( j = 0; j < level.maxclients; j++ )
			{
				gentity_t *other = &g_entities[j];
				vec3_t diff;

				if ( other == ent || !other->inuse || !other->client ||
					other->s.number < 0 || other->s.number >= MAX_CLIENTS ||
					other->client->pers.connected != CON_CONNECTED ||
					other->client->sess.sessionTeam == TEAM_SPECTATOR ||
					other->client->ps.pm_type == PM_DEAD )
				{
					continue;
				}

				VectorSubtract( other->client->ps.origin, ents[i]->s.origin, diff );
				gap[i] = min( gap[i], VectorLength( diff ) );
			}

			if ( gap[i] > bestGap )
			{
				bestGap = gap[i];
				best = i;
			}

			if ( gap[i] >= MOBA_SPAWN_CLEAR )
			{
				clearSpots[clear++] = i;
			}
		}

		if ( clear > 0 )
		{
			best = clearSpots[Q_irand( 0, clear - 1 )];
		}

		spot = ents[best];
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

// In the captain stage the whole pool is listed with its state, in the assign
// stage only the heroes the captain picked for the team of the reader, numbered
// the way !pick expects them.
static void MOBA_ListHeroes( gentity_t *ent )
{
	int i;
	team_t team = ent->client->sess.sessionTeam;
	qboolean assign = ( mobaPhase == MOBA_PHASE_DRAFT_ASSIGN ) ? qtrue : qfalse;

	if ( assign && ( team == TEAM_RED || team == TEAM_BLUE ) )
	{
		// only the pool of the team, but with the numbers of the full list so
		// the window and the chat can never disagree about a hero
		MOBA_Self( ent, "^3Heroes your captain picked for the %s team:",
			team == TEAM_RED ? "^1red^7" : "^4blue^7" );
	}

	for ( i = 0; i < mobaNumHeroes; i++ )
	{
		const char *state = "";

		if ( assign && mobaHeroBanned[i] )
		{
			continue;	// a banned hero is in no pool, the board already shows it
		}

		if ( mobaHeroBanned[i] )
		{
			state = " ^8(banned)^7";
		}
		else if ( mobaHeroTeam[i] == TEAM_RED )
		{
			state = " ^1(red)^7";
		}
		else if ( mobaHeroTeam[i] == TEAM_BLUE )
		{
			state = " ^4(blue)^7";
		}
		else if ( assign && ( team == TEAM_RED || team == TEAM_BLUE ) )
		{
			continue;	// not picked by the captains, so not in this pool
		}
		else if ( assign && MOBA_HeroTaken( i ) )
		{
			// the pool listing has no colour to show, "taken" is the marker there
			state = " ^8(taken)^7";
		}

		MOBA_Self( ent, "^3%2i^7 - %s (^5%s^7)%s",
			i + 1, mobaHeroes[i].name, mobaHeroes[i].role, state );
	}

	if ( assign && ( team == TEAM_RED || team == TEAM_BLUE ) )
	{
		MOBA_Self( ent, "^3!pick N^7 takes one of them for you" );
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

//=========================================================================
// Draft commands
//=========================================================================

// Both stages work on the same numbering: N is the number the hero has in the
// full list, which is also the number the client shows on its tile and the
// number the assign stage draws the pool with. The pool is only a filter, not a
// second index space, so a player cannot be told "3" by !heroes and have the
// click on tile 3 mean something else.
static qboolean MOBA_DraftHeroFromArg( gentity_t *ent, const char *arg, int *heroId )
{
	int id;

	if ( arg == NULL || arg[0] == '\0' )
	{
		MOBA_CPSelf( ent, "Which hero? See: !heroes\n" );
		return qfalse;
	}

	id = atoi( arg ) - 1;

	// "!pick 0" and a mistyped word both arrive here as -1, the hero array is
	// indexed with the result
	if ( id < 0 || id >= mobaNumHeroes )
	{
		MOBA_CPSelf( ent, "Bad hero number! See: !heroes\n" );
		return qfalse;
	}

	*heroId = id;
	return qtrue;
}

static void MOBA_DraftPick( gentity_t *ent, const char *arg )
{
	int heroId;

	if ( mobaPhase != MOBA_PHASE_DRAFT && mobaPhase != MOBA_PHASE_DRAFT_ASSIGN )
	{
		MOBA_CPSelf( ent, "Heroes can only be picked in the draft!\n" );
		return;
	}

	if ( mobaPhase == MOBA_PHASE_DRAFT && !MOBA_IsActingCaptain( ent ) )
	{
		MOBA_CPSelf( ent, "Not your turn - only the captains ban and pick.\n" );
		return;
	}

	if ( mobaPlayers[ent->s.number].heroId >= 0 )
	{
		MOBA_CPSelf( ent, "You already have a hero!\n" );
		return;
	}

	if ( !MOBA_DraftHeroFromArg( ent, arg, &heroId ) )
	{
		return;
	}

	if ( mobaPhase == MOBA_PHASE_DRAFT )
	{
		if ( mobaDraftPlan[mobaDraftStep] != MOBA_DRAFT_PICK )
		{
			MOBA_CPSelf( ent, "This turn is a ban - use ^3!ban N^7\n" );
			return;
		}

		if ( !MOBA_DraftCommit( heroId, "chosen" ) )
		{
			MOBA_CPSelf( ent, "%s is not available! See: !heroes\n",
				mobaHeroes[heroId].name );
		}
		return;
	}

	// assign stage: taking a hero out of the pool of the team
	if ( ent->client->sess.sessionTeam != TEAM_RED &&
		ent->client->sess.sessionTeam != TEAM_BLUE )
	{
		MOBA_CPSelf( ent, "You are not playing this match.\n" );
		return;
	}

	if ( mobaHeroBanned[heroId] || mobaHeroTeam[heroId] != ent->client->sess.sessionTeam )
	{
		MOBA_CPSelf( ent, "%s is not in the pool of your team! See: !heroes\n",
			mobaHeroes[heroId].name );
		return;
	}

	if ( MOBA_HeroTaken( heroId ) )
	{
		MOBA_CPSelf( ent, "%s is already taken!\n", mobaHeroes[heroId].name );
		return;
	}

	mobaPlayers[ent->s.number].heroId = heroId;
	MOBA_ApplyHeroModel( ent );
	MOBA_CPSelf( ent, "Hero picked: ^5%s^7!\n", mobaHeroes[heroId].name );
	MOBA_LogLine( va( "%s plays %s", ent->client->pers.netname,
		mobaHeroes[heroId].name ), ent );

	if ( MOBA_EveryoneHasHero() )
	{
		MOBA_CPAll( "^2Lineup complete^7 - to the shop\n" );
	}
}

static void MOBA_DraftBan( gentity_t *ent, const char *arg )
{
	int heroId;

	if ( mobaPhase != MOBA_PHASE_DRAFT )
	{
		MOBA_CPSelf( ent, "Heroes are only banned in the draft!\n" );
		return;
	}

	if ( !MOBA_IsActingCaptain( ent ) )
	{
		MOBA_CPSelf( ent, "Not your turn - only the captains ban and pick.\n" );
		return;
	}

	if ( mobaDraftPlan[mobaDraftStep] != MOBA_DRAFT_BAN )
	{
		MOBA_CPSelf( ent, "This turn is a pick - use ^3!pick N^7\n" );
		return;
	}

	if ( !MOBA_DraftHeroFromArg( ent, arg, &heroId ) )
	{
		return;
	}

	if ( !MOBA_DraftCommit( heroId, "chosen" ) )
	{
		MOBA_CPSelf( ent, "%s is not available! See: !heroes\n", mobaHeroes[heroId].name );
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
		MOBA_Self( ent, "^3Commands:^7 !heroes !pick N !ban N !buy N|code !buyall !upgrade N "
			"!buyback !1-!4 (abilities) !status\n"
			"^3Draft:^7 the two captains get !ban N and !pick N, everybody takes one "
			"hero out of the pool of his team afterwards, ^3!draft^7 brings the hero "
			"window back\n"
			"^3The shop is a window:^7 press ^3B^7 in the buy phase, pick a tab "
			"(^3Defence^7, ^3Attack^7, ^3Consumables^7) and buy with a left click, "
			"^3B^7 or ^3ESC^7 closes it" );
		return qtrue;
	}
	if ( !Q_stricmp( cmd, "!draft" ) )
	{
		// ESC cannot close the hero window any more, this is the way back for the
		// cases the client cannot see, a window that got lost on a cgame reload or
		// a client that joined in the middle of a phase
		if ( mobaPhase != MOBA_PHASE_DRAFT && mobaPhase != MOBA_PHASE_DRAFT_ASSIGN )
		{
			MOBA_Self( ent, "^3No draft is running^7 - the window opens by itself at "
				"the start of the next draft\n" );
			return qtrue;
		}

		trap->SendServerCommand( ent->s.number, "mobaDraftOpen" );
		MOBA_Self( ent, "^3The hero window is back.^7 It stays open until the draft "
			"is over\n" );
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
		MOBA_DraftPick( ent, arg1 );
		return qtrue;
	}

	if ( !Q_stricmp( cmd, "!ban" ) )
	{
		MOBA_DraftBan( ent, arg1 );
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


// The hero definitions themselves live in moba_content.h so the cgame can
// render the same numbers in the select window. The server works on its own
// writable copy, everything that changes at runtime (ban, team, owner) is kept
// in the mobaHeroBanned/mobaHeroTeam arrays next to the draft code.
static void MOBA_LoadHeroes( void )
{
	int i;

	for ( i = 0; i < MOBA_MAX_HEROES; i++ )
	{
		mobaHeroes[i] = mobaHeroTable[i];
	}

	mobaNumHeroes = MOBA_MAX_HEROES;
}
