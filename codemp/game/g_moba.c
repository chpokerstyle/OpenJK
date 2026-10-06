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
static int MOBA_MissingHeroCount( void );
static void MOBA_GiveHero( gentity_t *ent, int heroId );
static qboolean MOBA_HeroTaken( int heroId );
static qboolean MOBA_IsHostile( gentity_t *caster, gentity_t *targ );
static qboolean MOBA_IsAlly( gentity_t *caster, gentity_t *targ );
static void MOBA_BreakInvis( gentity_t *attacker );
static void MOBA_ClearInvis( int clientNum );
static void MOBA_TickItems( int clientNum );

// A charged ability fires from a small magazine: every cast spends one charge
// and only the last one starts the long cooldown that refills the magazine.
#define MOBA_PROJECTILE_CHARGES		4
#define MOBA_FLAME_GRACE_MS			300		// the client repeats the channel every ~200ms, this covers a late repeat
#define MOBA_FLAME_MAX_TICK_MS		500		// a stalled frame must not land one huge damage tick
#define MOBA_FIREBALL_SLOW_PCT		0.10f	// movement slow of a fireball hit
#define MOBA_FIREBALL_SLOW_MS		4000

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

// moba_mode, mirrored by the CG_MOBA_MODE_* values of cg_moba.c
#define MOBA_MODE_CAPTAIN	0
#define MOBA_MODE_ALLPICK	1

// The game type of the Create a game menu is the input for this, and it is read
// in G_InitGame. The mode is kept in a variable of its own because a cvar written
// through the trap is not visible to the cvar pointer in the same frame, and the
// mode is needed while the game is set up.
static int mobaMode = MOBA_MODE_ALLPICK;

static qboolean MOBA_ModeAllPick( void )
{
	return ( mobaMode == MOBA_MODE_ALLPICK ) ? qtrue : qfalse;
}

static const char *MOBA_ModeName( void )
{
	return MOBA_ModeAllPick() ? "All pick" : "Captain draft";
}

static qboolean mobaEnabled = qfalse;
static mobaPhase_t mobaPhase = MOBA_PHASE_LOBBY;
static int mobaPhaseEnd = 0;
static int mobaRound = 0;
static int mobaRedAlive = 0, mobaBlueAlive = 0;
static int mobaRedPlayers = 0, mobaBluePlayers = 0;

// Temporary debug breadcrumb for the BUY -> FIGHT crash hunt. Prints straight
// to the console so the lines survive a hard crash, unlike the buffered
// games.log. Enabled by moba_log like every MOBA diagnostic line.
static void MOBA_DbgPrint( const char *fmt, ... )
{
	va_list argptr;
	char msg[768];

	if ( !moba_log.integer )
	{
		return;
	}

	va_start( argptr, fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );

	trap->Print( "MOBADBG: %s\n", msg );
}

// ---- Lobby wait / ready / restart / ff ----
// The lobby gives everyone (up to sv_maxclients) time to connect before the
// draft launches. !ready counts every human on a team, bots are always ready
// (they cannot type). The wait ends when every expected player is ready or the
// moba_lobbyTime fallback fires, whichever comes first.
static qboolean mobaReady[MAX_CLIENTS];		// client said !ready this lobby
static int mobaLobbyStart = 0;				// level.time the lobby wait began (0 = idle)
static int mobaRestartVoteStart = 0;		// level.time of the first !restart vote
static qboolean mobaRestartVoted[MAX_CLIENTS];
static int mobaFFVoteStart = 0;				// level.time of the first !ff vote
static qboolean mobaFFVoted[MAX_CLIENTS];

mobaPlayer_t mobaPlayers[MAX_CLIENTS];

// [cloaked][viewer], so a repeat is skipped per pair: the two allies of a
// cloaked player get the same value but the enemies do not, and a spectator is
// nobody's enemy. Declared up here because the reset of a match has to clear it
// and that runs long before the cloak code.
static int mobaInvisSent[MAX_CLIENTS][MAX_CLIENTS];

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
//
// maxCount / cooldownMs / manaCost / effectAmount / durationMs describe a use. A
// cooldownMs of zero makes the item passive: it sits in a slot and gives its
// bonuses, but there is nothing to press.
mobaItem_t mobaItems[] = {
	{ "Sturdy Armor",		"armor",	"DEFENCE",		250,	50,	0,		0,
		"+50 armor",			1,	0,	0,	0,		0 },
	{ "Med Kit",			"med",		"CONSUMABLES",	200,	0,		0,		0,
		"heal 100 to a nearby ally",	1,	MOBA_ITEM_MEDKIT_CD,	0,	MOBA_ITEM_MEDKIT_HEAL,	0 },
	{ "Rage Rune",			"rage",	"ATTACK",		300,	0,		0,		20,
		"+20% damage",			1,	0,	0,	0,		0 },
	{ "Heavy Plate",		"heavy",	"DEFENCE",		500,	100,	50,	0,
		"+100 armor, +50 health",	1,	0,	0,	0,		0 },
	{ "Power Crystal",		"crystal",	"ATTACK",	650,	0,		50,	40,
		"+50 health, +40% damage",	1,	0,	0,	0,		0 },
	{ "Shadow Cloak",		"shadowcloak","DEFENCE",	400,	30,	0,		15,
		"+30 armor, +15% damage",	1,	0,	0,	0,		0 },
	{ "Grenade",			"grenade",	"CONSUMABLES",	75,		0,		0,		0,
		"throw a grenade, 4 per player",	MOBA_ITEM_GRENADE_MAX, MOBA_ITEM_GRENADE_CD, 0, 0, 0 },
	{ "Umbrella",			"umbrella","DEFENCE",		1000,	0,		0,		0,
		"+50 armor for 20 s",	1,	MOBA_ITEM_UMBRELLA_DUR + MOBA_ITEM_UMBRELLA_CD, MOBA_ITEM_UMBRELLA_MANA,
		MOBA_ITEM_UMBRELLA_ARMOR, MOBA_ITEM_UMBRELLA_DUR },
	{ "Invisibility Cloak",	"inviscloak","DEFENCE",	600,	0,		0,		0,
		"hidden from enemies, a ghost to allies",	1,	MOBA_ITEM_CLOAK_CD,	MOBA_ITEM_CLOAK_MANA,	0,	MOBA_ITEM_CLOAK_DUR }
};
int mobaNumItems = ARRAY_LEN( mobaItems );

//=========================================================================
static void MOBA_ResetPlayerSlots( mobaPlayer_t *p )
{
	int i;

	// memset leaves slotItem at 0, which reads as "the player already owns the
	// first item of the shop", so an empty slot has to be -1 and not 0.
	for ( i = 0; i < MOBA_ACTIVE_SLOTS; i++ )
	{
		p->slotItem[i] = -1;
		p->slotCount[i] = 0;
		p->slotCdReady[i] = 0;
		p->slotLastUse[i] = 0;
	}
}

static void MOBA_ResetPlayers( void )
{
	int i;

	memset( mobaPlayers, 0, sizeof( mobaPlayers ) );

	// Every client must be told about a cloak again after a restart: the table
	// holds what each viewer was last told, and a stale entry there would make
	// the new match believe the new cloak is already known.
	memset( mobaInvisSent, 0, sizeof( mobaInvisSent ) );

	// memset leaves heroId at 0, which reads as "hero 0 already picked" and makes
	// the whole draft phase a no-op, so mark every slot as empty explicitly.
	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		mobaPlayers[i].heroId = -1;
		mobaPlayers[i].mana = 0;
		mobaPlayers[i].lastManaTick = 0;
		MOBA_ResetPlayerSlots( &mobaPlayers[i] );
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
// per captain, each followed by two picks per captain. A captain picks exactly
// one hero per player of his own team, so the pool of a team always has a hero
// for everybody in the assign stage behind it, no matter how big the roster is.
// The number of bans shrinks if the roster is too big for the hero pool.
static void MOBA_BuildDraftPlan( void )
{
	int stage, i, seat, bans[MOBA_DRAFT_SEATS], picks[MOBA_DRAFT_SEATS];
	int banBudget, totalPicks, redPicks, bluePicks;

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

	// the plan walks the counters down to zero, the composition is logged before
	// that happens
	redPicks = picks[0];
	bluePicks = picks[1];

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

	// "one pick per player of the team" is a promise the plan has to keep, so it
	// is written down where a mismatch can be seen without a debugger
	MOBA_LogLine( va( "draft plan: %i steps - red %i player(s): %i ban(s) + %i pick(s), "
		"blue %i player(s): %i ban(s) + %i pick(s)", mobaDraftPlanLen,
		MOBA_TeamSize( TEAM_RED ), banBudget, redPicks,
		MOBA_TeamSize( TEAM_BLUE ), banBudget, bluePicks ), NULL );
}

// A roster that changes while the draft runs (a late joiner, a player that came
// back from a disconnect) has to keep the promise of the plan: every player of a
// team needs a hero, so a team that grew gets the missing picks appended to the
// end of the plan. The steps that already happened are never rewritten, and a
// team that shrank simply leaves an unused hero in its pool.
static void MOBA_ExtendDraftPlan( void )
{
	int seat, i, planned, want;

	for ( seat = 0; seat < MOBA_DRAFT_SEATS; seat++ )
	{
		want = MOBA_TeamSize( MOBA_SeatTeam( seat ) );
		planned = 0;

		for ( i = 0; i < mobaDraftPlanLen; i++ )
		{
			if ( mobaDraftActor[i] == seat && mobaDraftPlan[i] == MOBA_DRAFT_PICK )
			{
				planned++;
			}
		}

		for ( ; planned < want && mobaDraftPlanLen < MOBA_DRAFT_ACTIONS_MAX; planned++ )
		{
			mobaDraftPlan[mobaDraftPlanLen] = MOBA_DRAFT_PICK;
			mobaDraftActor[mobaDraftPlanLen] = seat;
			mobaDraftPlanLen++;
		}

		if ( planned < want )
		{
			MOBA_LogLine( va( "draft plan: %s team grew to %i player(s), "
				"the plan is full and cannot cover it",
				seat == 0 ? "red" : "blue", want ), NULL );
		}
	}
}

// How many picks of this seat the plan still holds, so a captain can be told
// what he is playing for instead of guessing from the step counter.
static int MOBA_DraftPicksLeft( int seat )
{
	int i, count = 0;

	for ( i = mobaDraftStep; i < mobaDraftPlanLen; i++ )
	{
		if ( mobaDraftActor[i] == seat && mobaDraftPlan[i] == MOBA_DRAFT_PICK )
		{
			count++;
		}
	}

	return count;
}

// A hero that is neither banned nor picked yet, picked at random. All pick never
// fills a team pool, so ownership lives in mobaPlayers there and has to be asked
// for separately: without it two players can end up with the same hero.
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
		if ( !mobaHeroBanned[i] && mobaHeroTeam[i] == TEAM_FREE &&
			!MOBA_HeroTaken( i ) )
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
	int seat, captain, secs, picksLeft;
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
	picksLeft = MOBA_DraftPicksLeft( seat );

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

	// a pick is not a one time thing: the captain of a team of n players picks n
	// heroes, one for every player of his team
	if ( !ban )
	{
		MOBA_CPSelf( ent, "That is ^3%i^7 of the ^3%i^7 hero(s) you pick for your team of ^3%i^7\n",
			picksLeft, picksLeft, MOBA_TeamSize( team ) );
	}
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

// How many connected players still have to take a hero. Both draft modes end on
// this number, so it is counted in one place.
static int MOBA_MissingHeroCount( void )
{
	int i, count = 0;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( MOBA_SlotActive( i ) && mobaPlayers[i].heroId < 0 )
		{
			count++;
		}
	}

	return count;
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
			mobaPlayers[i].flameUntil = 0;
			mobaPlayers[i].flameSlot = -1;
			ent->client->tempSpectate = 0;
			ent->client->respawnTime = 0;
			ent->client->ps.activeForcePass = 0;
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

// Number of invited players, i.e. the server's own slot limit. This is what
// "all players" means for the lobby: with three bots on an 8-slot server the
// 5 humans have to be ready, bots count as ready automatically.
static int MOBA_ReadyNeeded( void )
{
	return sv_maxclients.integer;
}

// Every client on a team counts, a bot always counts as ready. Only humans
// that typed !ready this lobby are ready; a bot cannot type.
static int MOBA_ReadyCount( void )
{
	int i, count = 0;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( !MOBA_SlotActive( i ) )
		{
			continue;
		}
		if ( g_entities[i].client->sess.sessionTeam != TEAM_RED &&
			g_entities[i].client->sess.sessionTeam != TEAM_BLUE )
		{
			continue;
		}
		if ( ( g_entities[i].r.svFlags & SVF_BOT ) || mobaReady[i] )
		{
			count++;
		}
	}

	return count;
}

static qboolean MOBA_AllReady( void )
{
	return ( MOBA_ReadyCount() >= MOBA_ReadyNeeded() ) ? qtrue : qfalse;
}

// The whole match (lineups, heroes, round) is thrown away and the lobby starts
// the wait for a fresh draft over again.
static void MOBA_RestartMatch( void )
{
	int i;

	MOBA_ResetPlayers();
	MOBA_ResetHeroPool();

	mobaDraftDone = qfalse;
	mobaRound = 0;
	mobaDraftFirst = 0;
	mobaAutoDraftNext = 0;

	mobaLobbyStart = 0;
	mobaRestartVoteStart = 0;
	mobaFFVoteStart = 0;
	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		mobaReady[i] = qfalse;
		mobaRestartVoted[i] = qfalse;
		mobaFFVoted[i] = qfalse;
	}

	MOBA_StartLobby();
	MOBA_CPAll( "^3The match was restarted^7 - a fresh draft begins after the lobby wait\n" );
}

// The current player list ("who is where and what") is written to
// moba_status.txt twice a second. The server launcher reads this file for its
// players/stats tab, it is deliberately flat so the C# side can parse it with
// a couple of string operations: line 1 is "phase:<n> round:<n>", every
// following line is "pipe"-separated fields, the player name is the last one.
static void MOBA_WriteStatusFile( void )
{
	fileHandle_t f;
	char buf[8192], clean[64], heroName[64];
	int len = 0, i;

	if ( trap->FS_Open( "moba_status.txt", &f, FS_WRITE ) < 0 )
	{
		return;
	}

	len += Com_sprintf( buf + len, sizeof( buf ) - len,
		"phase:%i round:%i\n", mobaPhase, mobaRound );

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent;
		const char *name;
		int n = 0, heroId;

		if ( !MOBA_SlotActive( i ) )
		{
			continue;
		}

		ent = &g_entities[i];
		name = ent->client->pers.netname;

		// strip colour codes (^7 and the like) so the launcher gets a clean name
		while ( *name && n < (int)sizeof( clean ) - 1 )
		{
			if ( *name == '^' && name[1] )
			{
				name += 2;
				continue;
			}
			clean[n++] = *name++;
		}
		clean[n] = '\0';

		heroId = mobaPlayers[i].heroId;
		if ( heroId >= 0 && heroId < MOBA_MAX_HEROES && mobaHeroes[heroId].name )
		{
			Q_strncpyz( heroName, mobaHeroes[heroId].name, sizeof( heroName ) );
		}
		else
		{
			Q_strncpyz( heroName, "-", sizeof( heroName ) );
		}

		len += Com_sprintf( buf + len, sizeof( buf ) - len,
			"%i|%i|%i|%s|%i|%i|%i|%i|%s\n",
			i,
			ent->client->sess.sessionTeam,
			( ent->r.svFlags & SVF_BOT ) ? 1 : 0,
			heroName,
			mobaPlayers[i].kills,
			mobaPlayers[i].deaths,
			mobaPlayers[i].level,
			mobaPlayers[i].gold,
			clean );
	}

	trap->FS_Write( buf, len, f );
	trap->FS_Close( f );
}

// Bots cannot type, so a whole team made of bots would be stuck waiting. A
// bot on a team votes with its team: as soon as every human of a team said
// !ff, the team concedes.
static qboolean MOBA_TeamFFComplete( team_t team )
{
	int i, humans = 0, voted = 0;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( !MOBA_SlotActive( i ) )
		{
			continue;
		}
		if ( g_entities[i].client->sess.sessionTeam != team )
		{
			continue;
		}
		if ( g_entities[i].r.svFlags & SVF_BOT )
		{
			continue;
		}
		humans++;
		if ( mobaFFVoted[i] )
		{
			voted++;
		}
	}

	return ( humans > 0 && voted >= humans ) ? qtrue : qfalse;
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

	if ( MOBA_ModeAllPick() )
	{
		// All pick has no captain job, no bans and no team pools: everybody picks
		// from the same board, so there is no plan to walk and the phase runs on
		// the players instead of on a step list.
		mobaPhaseEnd = level.time + moba_pickTime.integer * 1000;

		MOBA_CPAll( "^3All pick!^7 %i s - everybody takes a hero he likes: ^3!pick N^7 "
			"or click one in the window\n", moba_pickTime.integer );
		MOBA_CPAll( "^3All pick!^7 players without a hero: %i (%i s)\n",
			MOBA_MissingHeroCount(), moba_pickTime.integer );
		return;
	}

	mobaCaptain[0] = MOBA_ElectCaptain( TEAM_RED );
	mobaCaptain[1] = MOBA_ElectCaptain( TEAM_BLUE );

	MOBA_BuildDraftPlan();

	if ( mobaDraftPlanLen <= 0 )
	{
		MOBA_StartAssign();
		return;
	}

	mobaPhaseEnd = level.time + moba_pickTime.integer * 1000;

	MOBA_CPAll( "^3Hero draft!^7 %i steps, %i sec each - each captain picks one hero "
		"per player of his team\n", mobaDraftPlanLen, moba_pickTime.integer );
	MOBA_DraftPrompt();
}

// Every player of a team takes one hero out of the pool his captain picked. The
// stage ends as soon as nobody is left without a hero, so a quick team does not
// wait out the clock.
static void MOBA_StartAssign( void )
{
	int missing;

	// the lineup of the match is fixed from here on, the later rounds reuse it
	mobaDraftDone = qtrue;

	mobaPhase = MOBA_PHASE_DRAFT_ASSIGN;
	mobaPhaseEnd = level.time + moba_pickTime.integer * 1000;

	missing = MOBA_MissingHeroCount();

	MOBA_CPAll( "^3Hero assign!^7 %i player(s) still need a hero: ^3!pick N^7 - "
		"!heroes lists the pool of your team (%i s)\n", missing, moba_pickTime.integer );
}

static void MOBA_StartBuy( void )
{
	mobaPhase = MOBA_PHASE_BUY;
	mobaPhaseEnd = level.time + moba_buyTime.integer * 1000;

	MOBA_EnsureTeams();

	// the kill counter is per round and drives the timeout tie-break
	{
		int rk;

		for ( rk = 0; rk < level.maxclients && rk < MAX_CLIENTS; rk++ )
		{
			mobaPlayers[rk].roundKills = 0;
		}
	}

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
	int i;

	mobaPhase = MOBA_PHASE_FIGHT;
	mobaPhaseEnd = level.time + moba_roundTime.integer * 1000;
	MOBA_DbgPrint( "F1 StartFight entry" );

	// the buy phase already handed out the heroes and put everybody on the team
	// spawns, where they stayed frozen. The round starts from there, without a
	// second respawn that would yank the players back to the spawns.
	MOBA_FillMissingHeroes();
	MOBA_DbgPrint( "F2 heroes filled" );
	MOBA_CPAll( "^1ROUND %i - FIGHT!^7\n", mobaRound + 1 );
	MOBA_DbgPrint( "F3 banner sent" );

	// full mana pool for the round, the bar regens from there
	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( mobaPlayers[i].inuse && mobaPlayers[i].heroId >= 0 )
		{
			MOBA_DbgPrint( "F4 client %i hero %i mana %i", i,
				mobaPlayers[i].heroId, mobaHeroes[mobaPlayers[i].heroId].maxMana );
			mobaPlayers[i].mana = mobaHeroes[mobaPlayers[i].heroId].maxMana;
			mobaPlayers[i].manaFrac = 0;
			mobaPlayers[i].lastManaTick = level.time;
		}
	}
	MOBA_DbgPrint( "F9 StartFight done" );
}

static void MOBA_StartRoundEnd( void )
{
	team_t winner = TEAM_NUM_TEAMS;
	int i;
	int redKills = 0, blueKills = 0;

	if ( mobaRedAlive > mobaBlueAlive )
	{
		winner = TEAM_RED;
	}
	else if ( mobaBlueAlive > mobaRedAlive )
	{
		winner = TEAM_BLUE;
	}
	else
	{
		// equal alive count, which is the normal case when the round clock runs
		// out: the team that got more kills takes it
		for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
		{
			gentity_t *ent = &g_entities[i];

			if ( !MOBA_SlotActive( i ) )
			{
				continue;
			}
			if ( ent->client->sess.sessionTeam == TEAM_RED )
			{
				redKills += mobaPlayers[i].roundKills;
			}
			else if ( ent->client->sess.sessionTeam == TEAM_BLUE )
			{
				blueKills += mobaPlayers[i].roundKills;
			}
		}

		if ( redKills > blueKills )
		{
			winner = TEAM_RED;
		}
		else if ( blueKills > redKills )
		{
			winner = TEAM_BLUE;
		}
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

	mobaLobbyStart = 0;
	mobaRestartVoteStart = 0;
	mobaFFVoteStart = 0;
	memset( mobaReady, 0, sizeof( mobaReady ) );
	memset( mobaRestartVoted, 0, sizeof( mobaRestartVoted ) );
	memset( mobaFFVoted, 0, sizeof( mobaFFVoted ) );

	mobaEnabled = ( g_moba.integer != 0 ) ? qtrue : qfalse;

	// Picking one of the two MOBA game types in the Create a game menu is itself
	// the request to play the mod, so it turns the mod on even when g_moba is 0.
	// g_moba stays the switch for a plain team game (a server that only ever runs
	// moba.cfg), but a dedicated server started by ServerLauncher.exe reads
	// openjk.cfg alone and never execs moba.cfg, which used to leave g_moba at
	// its default of 0: no shop, no draft, no pick, and the client had nothing to
	// show. Choosing the mode in the menu can no longer be silently ignored.
	if ( !mobaEnabled &&
		( level.gametype == GT_MOBA_CAPTAIN || level.gametype == GT_MOBA_ALLPICK ) )
	{
		mobaEnabled = qtrue;
		trap->Cvar_Set( "g_moba", "1" );
	}

	if ( mobaEnabled )
	{
		// The two MOBA modes are two entries of the Create a game menu, so the
		// mode arrives as a game type. It is copied into moba_mode and the game is
		// turned into a plain team game right below, which keeps every gametype
		// check of the engine on GT_TEAM: a team based mode with two teams, which
		// is what the mod is built around. Any other game type keeps whatever
		// moba_mode says, so a plain team game runs in the default mode.
		mobaMode = moba_mode.integer;

		if ( level.gametype == GT_MOBA_CAPTAIN )
		{
			mobaMode = MOBA_MODE_CAPTAIN;
		}
		else if ( level.gametype == GT_MOBA_ALLPICK )
		{
			mobaMode = MOBA_MODE_ALLPICK;
		}

		trap->Cvar_Set( "moba_mode", va( "%i", mobaMode ) );

		// The mod is built around two teams, so refuse to run in a mode that has
		// none instead of silently behaving like free for all.
		if ( level.gametype != GT_TEAM )
		{
			MOBA_LogLine( va( "MOBA needs a team game, g_gametype %i -> %i",
				level.gametype, GT_TEAM ), NULL );
			level.gametype = GT_TEAM;
			trap->Cvar_Set( "g_gametype", va( "%i", GT_TEAM ) );
		}

		MOBA_CPAll( "^2Magic Wands^7: %s mode active (heroes: %i)\n",
			MOBA_ModeName(), mobaNumHeroes );

		// The log line is the only proof of what the match actually is, and it
		// goes through G_LogPrintf instead of trap->Print: printing from the game
		// module is a syscall the mod has no business making while the VM is being
		// set up, and autoexec.cfg keeps moba_log on so the line is always written.
		MOBA_LogLine( va( "mode ON: %s, %i heroes, g_moba %i, gametype %i, gold %i",
			MOBA_ModeName(), mobaNumHeroes, g_moba.integer, level.gametype,
			moba_startGold.integer ), NULL );

		MOBA_StartLobby();
	}
	else
	{
		MOBA_LogLine( va( "OFF: g_moba %i, gametype %i - set g_moba 1 or "
			"pick a MOBA game type", g_moba.integer, level.gametype ), NULL );
	}
}

//=========================================================================
// Dev aids - only active when the matching cvar is set, used to exercise the
// mod on a headless server where no real client can type chat commands.
//=========================================================================

// Resolves every draft step with a random hero instead of waiting out the clock,
// so a full ban/pick plan can be verified on a dedicated server. It also makes
// the captain election prefer a human, which is what allows the draft to be
// driven by hand while the bots hold the other captain jobs. All pick has no
// steps to walk, so one player without a hero per interval is served instead.
static void MOBA_RunAutoDraft( void )
{
	int i;

	if ( !moba_autodraft.integer || mobaPhase != MOBA_PHASE_DRAFT ||
		level.time < mobaAutoDraftNext )
	{
		return;
	}

	if ( MOBA_ModeAllPick() )
	{
		for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
		{
			gentity_t *ent = &g_entities[i];
			int heroId;

			if ( !MOBA_SlotActive( i ) || mobaPlayers[i].heroId >= 0 )
			{
				continue;
			}
			if ( ent->client->sess.sessionTeam != TEAM_RED &&
				ent->client->sess.sessionTeam != TEAM_BLUE )
			{
				continue;
			}

			heroId = MOBA_DraftFreeHero();
			if ( heroId < 0 )
			{
				break;
			}

			mobaAutoDraftNext = level.time + moba_autodraftEvery.integer;
			MOBA_CPAll( "^2PICK^7 - %s (auto) takes ^5%s^7\n",
				ent->client->pers.netname, mobaHeroes[heroId].name );
			MOBA_GiveHero( ent, heroId );
			return;
		}

		return;
	}

	if ( mobaDraftStep >= mobaDraftPlanLen )
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
static char mobaLastDraftSent[MAX_CLIENTS][192];
static char mobaAbilitiesSent[MAX_CLIENTS][96];
static int mobaAbilitiesNext[MAX_CLIENTS];
static int  mobaOwnerHeroSent[MAX_CLIENTS];			// hero the pushed owner name belongs to, -1 = none
static char mobaOwnerNameSent[MAX_CLIENTS][MAX_NETNAME];	// last owner name pushed for that hero
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
	char buf[96];
	int secs;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS ||
		!ent->inuse || !ent->client ||
		ent->client->pers.connected != CON_CONNECTED || !mobaPlayers[clientNum].inuse )
	{
		return;
	}

	secs = ( mobaPhaseEnd > level.time ) ? ( mobaPhaseEnd - level.time + 999 ) / 1000 : 0;

	// phase seconds gold itemMask level, then the two item slots as
	// item count cdLeft triples. An empty slot travels as item -1.
	Com_sprintf( buf, sizeof( buf ), "%i %i %i %i %i %i %i %i %i %i %i",
		mobaPhase, secs, mobaPlayers[clientNum].gold,
		mobaPlayers[clientNum].itemMask, mobaPlayers[clientNum].level,
		mobaPlayers[clientNum].slotItem[0],
		mobaPlayers[clientNum].slotCount[0],
		( mobaPlayers[clientNum].slotCdReady[0] > level.time ) ?
			( mobaPlayers[clientNum].slotCdReady[0] - level.time ) : 0,
		mobaPlayers[clientNum].slotItem[1],
		mobaPlayers[clientNum].slotCount[1],
		( mobaPlayers[clientNum].slotCdReady[1] > level.time ) ?
			( mobaPlayers[clientNum].slotCdReady[1] - level.time ) : 0 );

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
// "mobaDraft bannedLo bannedHi redLo redHi blueLo blueHi takenLo takenHi
//  action canAct seconds myHero myTeam step steps mode".
//
// The four hero sets are 64 bit masks, so each one travels as a low and a high
// half. Thirty heroes would still fit into an int, but the board grows with the
// content and a 32 bit mask would silently wrap once a thirty third hero is
// added; the split needs no format change for the next thirty years. action is
// 0 nothing to do, 1 ban, 2 pick and canAct is the only field the client is not
// allowed to guess, because "whose turn is it" changes whenever a captain times
// out or disconnects.
//=========================================================================
static void MOBA_PushDraftState( int clientNum )
{
	gentity_t *ent = &g_entities[clientNum];
	char buf[192];
	unsigned long long banned = 0, red = 0, blue = 0, taken = 0;
	int i, action, canAct, secs, myTeam;

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
				"mobaDraft \"0 0 0 0 0 0 0 0 0 0 0 0 -1 0 0 0\"" );
		}
		return;
	}

	for ( i = 0; i < mobaNumHeroes && i < MOBA_MAX_HEROES; i++ )
	{
		if ( mobaHeroBanned[i] )
		{
			banned |= ( 1ULL << i );
		}
		else if ( mobaHeroTeam[i] == TEAM_RED )
		{
			red |= ( 1ULL << i );
		}
		else if ( mobaHeroTeam[i] == TEAM_BLUE )
		{
			blue |= ( 1ULL << i );
		}
	}

	// the pool masks only say which heroes a team may hand out, not which of them
	// a player already took. Without this the window cannot tell a free hero from
	// a taken one and the confirm button would offer a hero the server refuses.
	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		if ( mobaPlayers[i].inuse && mobaPlayers[i].heroId >= 0 && mobaPlayers[i].heroId < MOBA_MAX_HEROES )
		{
			taken |= ( 1ULL << mobaPlayers[i].heroId );
		}
	}

	action = 0;
	canAct = 0;

	if ( mobaPhase == MOBA_PHASE_DRAFT && !MOBA_ModeAllPick() )
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
		// all pick hands heroes out of the shared board and the assign stage out
		// of the pool of the team, in both cases the action is always a pick
		action = 2;
		canAct = ( mobaPlayers[clientNum].heroId < 0 &&
			( ent->client->sess.sessionTeam == TEAM_RED ||
				ent->client->sess.sessionTeam == TEAM_BLUE ) ) ? 1 : 0;
	}

	secs = ( mobaPhaseEnd > level.time ) ? ( mobaPhaseEnd - level.time + 999 ) / 1000 : 0;
	myTeam = ent->client->sess.sessionTeam;

	// the mode travels with the board, so the window knows whether the heroes it
	// shows are a shared pool or the pool of the own team
	Com_sprintf( buf, sizeof( buf ),
		"%u %u %u %u %u %u %u %u %i %i %i %i %i %i %i %i",
		(unsigned)( banned & 0xFFFFFFFFULL ), (unsigned)( banned >> 32 ),
		(unsigned)( red & 0xFFFFFFFFULL ), (unsigned)( red >> 32 ),
		(unsigned)( blue & 0xFFFFFFFFULL ), (unsigned)( blue >> 32 ),
		(unsigned)( taken & 0xFFFFFFFFULL ), (unsigned)( taken >> 32 ),
		action, canAct, secs,
		mobaPlayers[clientNum].heroId, myTeam, mobaDraftStep, mobaDraftPlanLen,
		MOBA_ModeAllPick() ? MOBA_MODE_ALLPICK : MOBA_MODE_CAPTAIN );

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
// The draft board can say "you" for a hero only to the player who owns it. To
// every other client the same tile has to name the owner instead, and only the
// server knows the pair of hero and player, so it pushes it. One line per owner
// and only when a pair changed, which is a handful of lines per match.
//=========================================================================
static void MOBA_CleanName( const char *in, char *out, int outSize )
{
	int i = 0;

	if ( outSize <= 0 )
	{
		return;
	}

	for ( ; in && *in && i < outSize - 1; in++ )
	{
		char c = *in;

		// the name travels inside a quoted console command: a quote, a
		// semicolon or a backslash would end the command or start a new one
		if ( c == '"' || c == ';' || c == '\\' || c == '$' || c < 32 )
		{
			continue;
		}

		out[i++] = c;
	}

	out[i] = '\0';
}

static void MOBA_PushDraftOwners( void )
{
	int i;

	if ( mobaPhase != MOBA_PHASE_DRAFT && mobaPhase != MOBA_PHASE_DRAFT_ASSIGN )
	{
		for ( i = 0; i < MAX_CLIENTS; i++ )
		{
			mobaOwnerHeroSent[i] = -1;
			mobaOwnerNameSent[i][0] = '\0';
		}
		return;
	}

	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		gentity_t *ent = &g_entities[i];
		mobaPlayer_t *p = &mobaPlayers[i];
		char name[MAX_NETNAME], clean[MAX_NETNAME];

		if ( !p->inuse || p->heroId < 0 || p->heroId >= MOBA_MAX_HEROES ||
			!ent->inuse || !ent->client ||
			ent->client->pers.connected != CON_CONNECTED )
		{
			mobaOwnerHeroSent[i] = -1;
			mobaOwnerNameSent[i][0] = '\0';
			continue;
		}

		Q_strncpyz( name, ent->client->pers.netname, sizeof( name ) );
		MOBA_CleanName( name, clean, sizeof( clean ) );

		if ( !clean[0] )
		{
			continue;
		}

		if ( mobaOwnerHeroSent[i] == p->heroId &&
			Q_stricmp( mobaOwnerNameSent[i], clean ) == 0 )
		{
			continue;
		}

		mobaOwnerHeroSent[i] = p->heroId;
		Q_strncpyz( mobaOwnerNameSent[i], clean, sizeof( mobaOwnerNameSent[i] ) );

		// every client has to learn who owns the hero, not only the player next
		// to it, so the line is broadcast
		trap->SendServerCommand( -1, va( "mobaDraftOwner \"%i %s\"", p->heroId, clean ) );
	}
}

//=========================================================================
// Regen: the mana bar fills up slowly in the fight phase, always, so a player
// that waits a few seconds gets his pool back. Regeneration stays active while
// the match runs (a drained pool in the shop would feel pointless to waste on
// nothing). Ticking every frame and adding a fractional amount is heavy, so the
// regen is computed from the elapsed time since the last tick instead.
//=========================================================================
static void MOBA_TickMana( int clientNum )
{
	mobaPlayer_t *p = &mobaPlayers[clientNum];
	int maxMana, elapsed;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS || !p->inuse ||
		p->heroId < 0 || p->heroId >= mobaNumHeroes )
	{
		return;
	}

	if ( !p->lastManaTick )
	{
		p->lastManaTick = level.time;
		return;
	}

	elapsed = level.time - p->lastManaTick;
	p->lastManaTick = level.time;

	if ( elapsed <= 0 )
	{
		return;
	}

	maxMana = mobaHeroes[p->heroId].maxMana;

	// a 50ms tick would give 50*10/1000 == 0, so the regen rate is accumulated
	// in manaFrac as milli-mana and only whole points are moved into the pool
	p->manaFrac += elapsed * MOBA_MANA_REGEN_PER_SEC;
	p->mana += p->manaFrac / 1000;
	p->manaFrac %= 1000;

	if ( p->mana > maxMana )
	{
		p->mana = maxMana;
	}
}

//=========================================================================
// Pushes the four ability slots of one client to its own cgame:
// "mobaAbilities heroId cd0 cd1 cd2 cd3 lv0 lv1 lv2 lv3 mana maxMana
//  eff0 eff1 eff2 eff3".
//
// The hero id is repeated even though the draft push already carries it,
// because the bar in the fight phase has to know which hero it draws and the
// hero is not part of the shop state. cd is the milliseconds a slot still has
// to wait, lv the rank the player bought, so the client can grey the slot out
// and count down without asking anything. eff is the milliseconds a running
// effect of that slot still lasts, it drives the little buff icons above the
// bar.
//
// A running cooldown changes the numbers on every frame, so the push is rate
// limited to five times a second; the client counts the remaining time down on
// its own between the pushes, exactly like the phase timer.
//=========================================================================
static void MOBA_PushAbilityState( int clientNum )
{
	gentity_t *ent = &g_entities[clientNum];
	mobaPlayer_t *p = &mobaPlayers[clientNum];
	char buf[96];
	int i, cd[4], lv[4], eff[4];

	// The wire format stays fifteen numbers wide no matter how many abilities a
	// hero has, the slots that do not exist any more simply travel as zero.
	memset( cd, 0, sizeof( cd ) );
	memset( lv, 0, sizeof( lv ) );
	memset( eff, 0, sizeof( eff ) );

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
			trap->SendServerCommand( ent->s.number,
				"mobaAbilities \"-1 0 0 0 0 0 0 0 0 0 0 0 0 0 0\"" );
		}
		return;
	}

	for ( i = 0; i < MOBA_ABILITIES_PER_HERO; i++ )
	{
		const mobaAbility_t *ab = &mobaHeroes[p->heroId].abilities[i];
		int end = 0;

		cd[i] = ( p->cdReady[i] > level.time ) ? ( p->cdReady[i] - level.time ) : 0;
		lv[i] = p->abilityLevel[i];

		// the effect time is only meaningful for the slots that leave something
		// running on the caster, the damage slots report zero
		switch ( ab->type )
		{
		case AB_BUFF:		end = p->buffEndTime;		break;
		case AB_SHIELD:		end = p->shieldEndTime;		break;
		case AB_MAGICRESIST:end = p->magicResistEndTime;	break;
		case AB_FLAME:		end = p->flameUntil;		break;
		default:			end = 0;					break;
		}

		eff[i] = ( end > level.time ) ? ( end - level.time ) : 0;
	}

	Com_sprintf( buf, sizeof( buf ), "%i %i %i %i %i %i %i %i %i %i %i %i %i %i %i",
		p->heroId,
		cd[0], cd[1], cd[2], cd[3], lv[0], lv[1], lv[2], lv[3],
		p->mana, mobaHeroes[p->heroId].maxMana,
		eff[0], eff[1], eff[2], eff[3] );

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

//=========================================================================
// The flame channel. The client repeats the cast while the key is held, every
// repeat only pushed flameUntil forward; the actual damage and the actual mana
// cost are paid here, once per frame, so holding the key for twice as long
// costs twice as much and deals twice as much.
//=========================================================================
static void MOBA_TickFlame( int clientNum )
{
	mobaPlayer_t *p = &mobaPlayers[clientNum];
	gentity_t *ent;
	const mobaAbility_t *ab;
	int dt, i;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS || !p->inuse )
	{
		return;
	}

	if ( p->flameUntil <= level.time )
	{
		// the channel just ended: douse the hand effect and drop the casting
		// pose, otherwise the fire would keep burning on an idle player
		if ( p->flameSlot >= 0 )
		{
			ent = &g_entities[clientNum];

			if ( ent->inuse && ent->client )
			{
				ent->client->ps.activeForcePass = 0;

				if ( ent->client->ps.torsoAnim == BOTH_FORCELIGHTNING_HOLD )
				{
					ent->client->ps.torsoAnim = BOTH_STAND1;
					ent->client->ps.torsoTimer = 0;
				}
			}
		}

		p->flameSlot = -1;
		return;
	}

	if ( p->heroId < 0 || p->heroId >= mobaNumHeroes )
	{
		p->flameUntil = 0;
		return;
	}

	if ( p->flameSlot < 0 || p->flameSlot >= MOBA_ABILITIES_PER_HERO )
	{
		p->flameUntil = 0;
		return;
	}

	ent = &g_entities[clientNum];

	if ( !ent->inuse || !ent->client )
	{
		return;
	}

	ab = &mobaHeroes[p->heroId].abilities[p->flameSlot];
	if ( ab->type != AB_FLAME )
	{
		p->flameUntil = 0;
		p->flameSlot = -1;
		return;
	}

	// out of mana: the fire dies right here, the next cast has to wait for it
	if ( p->mana <= 0 )
	{
		p->flameUntil = 0;
		p->flameSlot = -1;
		MOBA_CPSelf( ent, "%s went out: no mana\n", ab->name );
		return;
	}

	dt = level.time - p->flameTick;
	if ( dt <= 0 )
	{
		return;
	}
	if ( dt > MOBA_FLAME_MAX_TICK_MS )
	{
		dt = MOBA_FLAME_MAX_TICK_MS;
	}
	p->flameTick = level.time;

	// mana per second, accumulated as milli-mana so a short tick still costs
	// its exact share and only whole points leave the pool
	p->flameManaFrac += ab->manaCost * dt;
	{
		int mana = p->flameManaFrac / 1000;

		p->flameManaFrac %= 1000;

		if ( mana > p->mana )
		{
			mana = p->mana;
			p->flameUntil = 0;
		}
		p->mana -= mana;
	}

	// The visible flame. boba-fett.pk3 overrides effects/force/lightning.efx so
	// the force-lightning effect is drawn with fire shaders and the lightning
	// sound is a fire crackle. The cgame already draws that effect from the
	// caster's hand whenever activeForcePass is set, so we light that flag and
	// hold the lightning pose; extra puffs are dropped along the cone so the
	// stream reaches the target instead of hugging the hand.
	{
		vec3_t fwd, org, end, delta;
		float reach;
		int fx = G_EffectIndex( "force/lightning" );

		AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );

		VectorCopy( ent->client->ps.origin, org );
		org[2] += ent->client->ps.viewheight - 6.0f;
		VectorMA( org, 6.0f, fwd, org );

		// where the stream stops: the first wall or the end of the range
		VectorMA( org, ab->range, fwd, end );
		{
			trace_t tr;

			trap->Trace( &tr, org, vec3_origin, vec3_origin, end, ent->s.number, MASK_SHOT, qfalse, 0, 0 );
			VectorCopy( tr.endpos, end );
		}

		// the muzzle burst rides every frame, the puffs further down the cone
		// and the sound are throttled so a held channel does not swamp the
		// temp-entity pool
		G_PlayEffectID( fx, org, fwd );

		VectorSubtract( end, org, delta );
		reach = VectorLength( delta );

		if ( level.time >= p->flameFxTime )
		{
			vec3_t mid;

			p->flameFxTime = level.time + 100;

			if ( reach > 80.0f )
			{
				VectorMA( org, 0.5f, delta, mid );
				G_PlayEffectID( fx, mid, fwd );
			}

			G_PlayEffectID( fx, end, fwd );
		}

		if ( level.time >= p->flameSndTime )
		{
			p->flameSndTime = level.time + 400;
			G_Sound( ent, CHAN_AUTO, G_SoundIndex( "sound/weapons/force/lightning" ) );
		}

		ent->client->ps.activeForcePass = 1;

		G_SetAnim( ent, NULL, SETANIM_TORSO, BOTH_FORCELIGHTNING_HOLD,
			SETANIM_FLAG_OVERRIDE | SETANIM_FLAG_HOLD, 0 );
		ent->client->ps.torsoTimer = 1;
	}

	// damage per second over every hostile caught in the cone in front
	p->flameDmgAcc += ab->baseEffect * dt;
	{
		int dmg = p->flameDmgAcc / 1000;

		p->flameDmgAcc %= 1000;

		if ( dmg > 0 )
		{
			vec3_t fwd;

			AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );

			for ( i = 0; i < level.maxclients; i++ )
			{
				gentity_t *target = &g_entities[i];
				vec3_t dir;
				float dist;

				if ( !target->inuse || !target->client || !MOBA_IsHostile( ent, target ) )
				{
					continue;
				}

				VectorSubtract( target->client->ps.origin, ent->client->ps.origin, dir );
				dist = VectorLength( dir );
				if ( dist > ab->range || dist < 1.0f )
				{
					continue;
				}

				// a cone, not a ball: the flame only burns what stands in front
				VectorScale( dir, 1.0f / dist, dir );
				if ( DotProduct( dir, fwd ) < 0.5f )
				{
					continue;
				}

				G_Damage( target, ent, ent, dir, target->client->ps.origin, dmg, 0, MOD_MOBA );
			}
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
	MOBA_RunAutoDraft();
	MOBA_RunTestKill();
	MOBA_RunTestKick();

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		MOBA_TickMana( i );
		MOBA_TickFlame( i );
		MOBA_TickItems( i );
		MOBA_PushShopState( i );
		MOBA_PushDraftState( i );
		MOBA_PushAbilityState( i );
	}

	MOBA_PushDraftOwners();

	// the launcher's players/stats tab polls this file, refresh it twice a second
	MOBA_WriteStatusFile();

	switch ( mobaPhase )
	{
	case MOBA_PHASE_LOBBY:
		if ( level.numConnectedClients >= 2 )
		{
			if ( !mobaLobbyStart )
			{
				mobaLobbyStart = level.time;
				MOBA_CPAll( "^3The match starts in %i s^7 - type ^3!ready^7 to begin "
					"as soon as everybody is here\n", moba_lobbyTime.integer );
			}

			// everyone expected (up to sv_maxclients) typed !ready, or the lobby
			// fallback time ran out: launch. The draft runs once per match, every
			// later round goes straight to the shop with the lineup of the first one.
			if ( MOBA_AllReady() ||
				level.time >= mobaLobbyStart + moba_lobbyTime.integer * 1000 )
			{
				mobaLobbyStart = 0;
				if ( mobaDraftDone )
				{
					MOBA_StartBuy();
				}
				else
				{
					MOBA_StartDraft();
				}
			}
		}
		else
		{
			mobaLobbyStart = 0;
		}
		break;

	case MOBA_PHASE_DRAFT:
		// a client that joins in the middle of the draft still has to end up on
		// a team, it is too late for its captain job but not for the match
		MOBA_EnsureTeams();

		if ( MOBA_ModeAllPick() )
		{
			// everybody picks from the same board, so the phase is over as soon
			// as the last player has a hero. Nobody has to act in a fixed order,
			// which is why there is no plan and no step counter here.
			if ( MOBA_EveryoneHasHero() )
			{
				MOBA_CPAll( "^2Lineup complete^7 - to the shop\n" );
				MOBA_StartBuy();
			}
			else if ( level.time >= mobaPhaseEnd )
			{
				MOBA_CPAll( "^3All pick is over, %i player(s) get a random hero^7\n",
					MOBA_MissingHeroCount() );
				MOBA_StartBuy();
			}
			break;
		}

		// a team that grew after the plan was built still needs a hero per player
		MOBA_ExtendDraftPlan();

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
		else if ( mobaPhaseEnd && level.time >= mobaPhaseEnd )
		{
			// the round clock ran out before a team was wiped out
			MOBA_CPAll( "^3The round time is up!\n" );
			MOBA_StartRoundEnd();
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

int MOBA_AdjustDamage( gentity_t *targ, gentity_t *attacker, gentity_t *inflictor, int meansOfDeath, int damage )
{
	mobaPlayer_t *p;
	float mult;

	if ( !mobaEnabled )
	{
		return damage;
	}

	// The target side runs first and stands on its own: a shield or a magic
	// resistance buff has to work no matter who dealt the blow, even against an
	// attacker that has no client slot at all (a missile's shooter is a player,
	// but scripted entities can deal damage too).
	if ( targ && targ->client && targ->s.number >= 0 && targ->s.number < MAX_CLIENTS )
	{
		mobaPlayer_t *t = &mobaPlayers[targ->s.number];

		// only ability and projectile damage counts as magic, a saber hit is not
		// reduced by the priest's blessing
		if ( meansOfDeath == MOD_MOBA && t->magicResistEndTime > level.time && damage > 0 )
		{
			damage = (int)( damage * 0.5f );
			if ( damage < 1 )
			{
				damage = 1;
			}
		}

		// the shield eats what it can and lets the rest through, a fully absorbed
		// hit deals no damage at all
		if ( t->shieldEndTime > level.time && t->shieldAmount > 0 && damage > 0 )
		{
			if ( damage <= t->shieldAmount )
			{
				t->shieldAmount -= damage;
				damage = 0;
			}
			else
			{
				damage -= t->shieldAmount;
				t->shieldAmount = 0;
			}
		}
	}

	if ( damage <= 0 )
	{
		return 0;
	}

	// vehicles and other scripted entities carry a client pointer but do not
	// sit in a client slot, they must never index the player array
	if ( !attacker || !attacker->client || targ == attacker ||
		attacker->s.number < 0 || attacker->s.number >= MAX_CLIENTS )
	{
		return damage;
	}

	p = &mobaPlayers[attacker->s.number];

	// the cloak only hides a player who stays off the damage lists, so the
	// first thing this player deals ends it
	MOBA_BreakInvis( attacker );

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

	// Heroes start without any armor. Everything the player walks around with has
	// to be bought in the shop or earned by the umbrella, so the base armor of the
	// hero table is only kept as a balance reference and is never handed out.
	armor = 0;

	// the umbrella is not a bonus in the item table, it is a temporary armor
	// pool that only exists while the shield is up
	if ( p->umbrellaEndTime > level.time )
	{
		armor += MOBA_ITEM_UMBRELLA_ARMOR;
	}

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

	if ( moba_rebrandModel.integer )
	{
		Info_SetValueForKey( userinfo, "model", want );
		trap->SetUserinfo( num, userinfo );
		ClientUserinfoChanged( num );
	}
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
		MOBA_CPSelf( ent, "^3Magic Wands^7 (%s): !heroes - hero list,\n"
			"^3!pick N^7 - hero, ^3!ban N^7 - draft ban,\n"
			"^3B^7 - open or close the shop window in the buy phase,\n"
			"^3ESC^7 closes the shop, a left click buys the item under it,\n"
			"!buy N|code - buy, !buyall - buy everything affordable,\n"
			"!upgrade N - upgrade ability, ^3Q E^7 - abilities, ^3C V^7 - item slots,\n"
			"!buyback - return after death, !status - stats, !help - all commands\n",
			MOBA_ModeName() );
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
		MOBA_ResetPlayerSlots( &mobaPlayers[ent->s.number] );

		// A cloak that was running dies with the client, so the viewers are told
		// to stop drawing a ghost, and both rows and the column of the slot are
		// cleared: a client that comes back into this slot would otherwise be
		// skipped as "already told" and would draw a body nobody else sees.
		MOBA_ClearInvis( ent->s.number );
		memset( mobaInvisSent[ent->s.number], 0, sizeof( mobaInvisSent[ent->s.number] ) );
		{
			int i;

			for ( i = 0; i < MAX_CLIENTS; i++ )
			{
				mobaInvisSent[i][ent->s.number] = 0;
			}
		}
		mobaReady[ent->s.number] = qfalse;
		mobaRestartVoted[ent->s.number] = qfalse;
		mobaFFVoted[ent->s.number] = qfalse;
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
	vp->deaths++;

	// a flame channel dies with its caster, otherwise the corpse would keep
	// breathing fire until the next respawn
	vp->flameUntil = 0;
	vp->flameSlot = -1;
	self->client->ps.activeForcePass = 0;

	// the cloak and the umbrella die with the body too, a respawn must not hand
	// out a free extra eight seconds of invisibility
	vp->invisEndTime = 0;
	vp->invisStartTime = 0;
	vp->umbrellaEndTime = 0;
	MOBA_ClearInvis( self->s.number );

	// never auto-respawn during a fight round
	self->client->respawnTime = level.time + MOBA_FALLBACK_RESPAWN;

	// let the fallen watch their team: a client with an active tempSpectate is
	// routed through the free spectator camera, so it can fly over the fight
	// until the next buy phase respawns everybody. Cleared again on respawn and
	// buyback.
	self->client->tempSpectate = level.time + MOBA_FALLBACK_RESPAWN;

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
		kp->kills++;

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

// The leap is a short teleport with a landing shock: the trace keeps the body
// out of the wall it would fly into, and everybody hostile caught at the landing
// spot is knocked off their feet and takes the landing damage.
static void MOBA_CastLeap( gentity_t *ent, const mobaAbility_t *ab )
{
	trace_t tr;
	vec3_t fwd, start, end, landOrigin;
	int i, dmg = MOBA_AbilityPower( ent, ab );

	// A forward dash: the view pitch is dropped so the leap always covers the
	// same ground, and the whole body box is traced instead of a point. A point
	// stops on the very surface of a wall and then the body pokes into it, which
	// is exactly how the hero used to get stuck in a texture or in another hero.
	AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
	fwd[2] = 0.0f;
	if ( VectorNormalize( fwd ) == 0.0f )
	{
		fwd[0] = 1.0f;
	}

	VectorCopy( ent->r.currentOrigin, start );
	VectorMA( start, ab->range, fwd, end );

	trap->Trace( &tr, start, ent->r.mins, ent->r.maxs, end, ent->s.number,
		MASK_PLAYERSOLID, qfalse, 0, 0 );

	if ( tr.startsolid || tr.allsolid )
	{
		MOBA_CPSelf( ent, "%s needs room to land\n", ab->name );
		return;
	}

	VectorCopy( tr.endpos, landOrigin );

	// if the dash stopped in mid air, look for the floor below so the hero does
	// not hang there and then drop out of the fight
	{
		trace_t down;

		VectorCopy( landOrigin, end );
		end[2] -= 256.0f;
		trap->Trace( &down, landOrigin, ent->r.mins, ent->r.maxs, end, ent->s.number,
			MASK_PLAYERSOLID, qfalse, 0, 0 );

		if ( down.fraction < 1.0f && !down.startsolid && !down.allsolid )
		{
			VectorCopy( down.endpos, landOrigin );
		}
	}

	// unlink, move, link, the same dance TeleportPlayer does: the entity has to
	// leave the world tree before its box moves, or it stays linked at the old
	// spot and everything else keeps colliding with the ghost
	trap->UnlinkEntity( (sharedEntity_t *)ent );
	G_SetOrigin( ent, landOrigin );
	VectorCopy( landOrigin, ent->client->ps.origin );
	VectorClear( ent->client->ps.velocity );
	ent->client->ps.eFlags ^= EF_TELEPORT_BIT;
	trap->LinkEntity( (sharedEntity_t *)ent );

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *target = &g_entities[i];
		vec3_t dir;

		if ( !target->inuse || !target->client || !MOBA_IsHostile( ent, target ) )
		{
			continue;
		}

		VectorSubtract( target->client->ps.origin, landOrigin, dir );
		if ( VectorLength( dir ) <= ab->radius )
		{
			G_Damage( target, ent, ent, dir, target->client->ps.origin, dmg, 0, MOD_MOBA );
			G_Knockdown( target );
		}
	}

	MOBA_CPSelf( ent, "%s! Landed with %i damage\n", ab->name, dmg );
}

// A shield is a pool of health that sits in front of the real one: it is spent
// by damage in MOBA_AdjustDamage and runs out on its own.
static void MOBA_CastShield( gentity_t *ent, const mobaAbility_t *ab )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	int amount = MOBA_AbilityPower( ent, ab );

	p->shieldAmount = amount;
	p->shieldEndTime = level.time + ab->durationMs;
	p->effectEndTime[1] = p->shieldEndTime;

	MOBA_CPSelf( ent, "%s! Shield absorbs %i for %.0f sec\n",
		ab->name, amount, ab->durationMs / 1000.0f );
}

// The fireball is a blaster bolt with a slower travel and a slow on impact. It
// is spawned exactly like WP_FireBlasterMissile does, only the classname and the
// method of death differ, and G_MissileImpact sees the classname to call back
// into MOBA_OnMissileImpact for the slow.
static void MOBA_CastProjectile( gentity_t *ent, const mobaAbility_t *ab )
{
	vec3_t fwd, start;
	gentity_t *missile;
	int dmg = MOBA_AbilityPower( ent, ab );

	AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
	VectorCopy( ent->client->ps.origin, start );
	start[2] += 24.0f;

	// half the blaster velocity, so the bolt is visibly on its way and can be
	// dodged instead of being an instant hit
	missile = CreateMissile( start, fwd, 1150.0f, 10000, ent, qfalse );

	if ( missile )
	{
		missile->classname = "moba_fireball";
		missile->s.weapon = WP_BLASTER;
		missile->damage = dmg;
		missile->dflags = 0;
		missile->methodOfDeath = MOD_MOBA;
		missile->clipmask = MASK_SHOT | CONTENTS_LIGHTSABER;
		missile->bounceCount = 8;
	}
}

// The flame is a channel, not a single cast: the client repeats the cast while
// the key is held and every repeat only extends the lit time. The mana and the
// damage then drip in MOBA_TickFlame, so the cost follows the time held.
static void MOBA_CastFlame( gentity_t *ent, const mobaAbility_t *ab, int slot )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];

	if ( p->flameUntil <= level.time || p->flameSlot != slot )
	{
		p->flameTick = level.time;
		p->flameDmgAcc = 0;
		p->flameManaFrac = 0;
		p->flameFxTime = 0;
	}

	p->flameSlot = slot;
	p->flameUntil = level.time + MOBA_FLAME_GRACE_MS;
}

// Silence does not touch the movement or the saber, it only sets the flag the
// cast path checks, so a silenced enemy can still run and swing but not cast.
static void MOBA_CastSilence( gentity_t *ent, const mobaAbility_t *ab )
{
	int i, count = 0;

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *target = &g_entities[i];
		vec3_t dir;

		if ( !target->inuse || !target->client || !MOBA_IsHostile( ent, target ) )
		{
			continue;
		}

		VectorSubtract( target->client->ps.origin, ent->client->ps.origin, dir );
		if ( VectorLength( dir ) <= ab->radius )
		{
			mobaPlayers[i].silenceEndTime = level.time + ab->durationMs;
			count++;
		}
	}

	MOBA_CPSelf( ent, "%s! Silenced %i enemies for %.0f sec\n",
		ab->name, count, ab->durationMs / 1000.0f );
}

static void MOBA_CastMagicResist( gentity_t *ent, const mobaAbility_t *ab )
{
	int i, count = 0;

	for ( i = 0; i < level.maxclients; i++ )
	{
		gentity_t *target = &g_entities[i];
		vec3_t dir;

		if ( !target->inuse || !target->client || !MOBA_IsAlly( ent, target ) )
		{
			continue;
		}

		VectorSubtract( target->client->ps.origin, ent->client->ps.origin, dir );
		if ( VectorLength( dir ) <= ab->radius )
		{
			mobaPlayers[i].magicResistEndTime = level.time + ab->durationMs;
			count++;
		}
	}

	MOBA_CPSelf( ent, "%s! %i allies take half magic damage for %.0f sec\n",
		ab->name, count, ab->durationMs / 1000.0f );
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

	// silenced: the one thing a silence stops is casting, the saber still swings
	if ( p->silenceEndTime > level.time )
	{
		MOBA_CPSelf( ent, "You are silenced for %i sec!\n",
			( p->silenceEndTime - level.time ) / 1000 + 1 );
		return;
	}

	// a charged ability refills as soon as its cooldown is over; the cooldown is
	// only ever started by the last charge, not by every shot
	if ( ab->type == AB_PROJECTILE && p->charges[slot] == 0 &&
		level.time >= p->cdReady[slot] )
	{
		p->charges[slot] = MOBA_PROJECTILE_CHARGES;
	}

	if ( p->charges[slot] >= 0 )
	{
		if ( p->charges[slot] <= 0 )
		{
			MOBA_CPSelf( ent, "%s is reloading (%i sec)\n",
				ab->name, ( p->cdReady[slot] - level.time ) / 1000 + 1 );
			return;
		}
	}
	else if ( level.time < p->cdReady[slot] )
	{
		MOBA_CPSelf( ent, "%s on cooldown (%i sec)\n",
			ab->name, ( p->cdReady[slot] - level.time ) / 1000 + 1 );
		return;
	}

	// the flame is paid for by the second while it burns, every other ability
	// pays its cost up front
	if ( ab->type == AB_FLAME )
	{
		if ( p->mana <= 0 )
		{
			MOBA_CPSelf( ent, "Out of mana for %s\n", ab->name );
			return;
		}
	}
	else
	{
		if ( p->mana < ab->manaCost )
		{
			MOBA_CPSelf( ent, "Not enough mana for %s (%i needed)\n",
				ab->name, ab->manaCost );
			return;
		}

		p->mana -= ab->manaCost;
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
	case AB_LEAP:
		MOBA_CastLeap( ent, ab );
		break;
	case AB_SHIELD:
		MOBA_CastShield( ent, ab );
		break;
	case AB_PROJECTILE:
		MOBA_CastProjectile( ent, ab );
		break;
	case AB_FLAME:
		MOBA_CastFlame( ent, ab, slot );
		break;
	case AB_SILENCE:
		MOBA_CastSilence( ent, ab );
		break;
	case AB_MAGICRESIST:
		MOBA_CastMagicResist( ent, ab );
		break;
	default:
		return;
	}

	// a charge is spent per cast and only the last one starts the cooldown; the
	// flame has no cooldown at all, it lives as long as its cost is paid
	if ( p->charges[slot] >= 0 )
	{
		p->charges[slot]--;
		if ( p->charges[slot] <= 0 )
		{
			p->cdReady[slot] = level.time + ab->cooldownMs;
		}
	}
	else if ( ab->type != AB_FLAME )
	{
		p->cdReady[slot] = level.time + ab->cooldownMs;
	}
}

//=========================================================================
// Slow and missile callbacks
//=========================================================================

// Called from ClientThink once the command of the frame is final. A slow only
// touches the two movement axes; looking, shooting and force stay untouched, so
// a slowed player can still fight, just not run.
void MOBA_ClientThink( gentity_t *ent )
{
	mobaPlayer_t *p;

	if ( !mobaEnabled || !ent || !ent->client )
	{
		return;
	}

	if ( ent->s.number < 0 || ent->s.number >= MAX_CLIENTS )
	{
		return;
	}

	p = &mobaPlayers[ent->s.number];

	if ( p->slowEndTime <= level.time )
	{
		if ( p->slowPct != 0.0f )
		{
			p->slowPct = 0.0f;
		}
		return;
	}

	if ( p->slowPct > 0.0f )
	{
		float keep = 1.0f - p->slowPct;

		if ( keep < 0.0f )
		{
			keep = 0.0f;
		}

		ent->client->pers.cmd.forwardmove = (int)( ent->client->pers.cmd.forwardmove * keep );
		ent->client->pers.cmd.rightmove = (int)( ent->client->pers.cmd.rightmove * keep );
	}
}

// The fireball carries its own classname, so G_MissileImpact calls back here the
// moment the bolt lands on a body and the slow is applied from the hit itself
// instead of being guessed out of a damage event.
void MOBA_OnMissileImpact( gentity_t *missile, gentity_t *other )
{
	gentity_t *owner;
	mobaPlayer_t *t;

	if ( !mobaEnabled || !missile || !other )
	{
		return;
	}

	if ( !other->client || other->s.number < 0 || other->s.number >= MAX_CLIENTS )
	{
		return;
	}

	owner = &g_entities[missile->r.ownerNum];

	// never slow a teammate, a friendly fire bolt should not be a tool
	if ( owner->client && OnSameTeam( owner, other ) )
	{
		return;
	}

	t = &mobaPlayers[other->s.number];

	// the strongest slow wins and refreshes, a weaker one never shortens it
	if ( MOBA_FIREBALL_SLOW_PCT >= t->slowPct || t->slowEndTime <= level.time )
	{
		t->slowPct = MOBA_FIREBALL_SLOW_PCT;
	}
	t->slowEndTime = level.time + MOBA_FIREBALL_SLOW_MS;
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
	// in all pick every player shares one board, so the listing is the full list
	// and a hero that somebody already owns has to be marked
	qboolean allPick = ( mobaPhase == MOBA_PHASE_DRAFT && MOBA_ModeAllPick() ) ?
		qtrue : qfalse;

	if ( allPick )
	{
		MOBA_Self( ent, "^3Heroes you can still take (all pick):" );
	}
	else if ( assign && ( team == TEAM_RED || team == TEAM_BLUE ) )
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
		else if ( ( assign || allPick ) && MOBA_HeroTaken( i ) )
		{
			// the pool listing has no colour to show, "taken" is the marker there
			state = " ^8(taken)^7";
		}

		MOBA_Self( ent, "^3%2i^7 - %s (^5%s^7)%s",
			i + 1, mobaHeroes[i].name, mobaHeroes[i].role, state );
	}

	if ( allPick )
	{
		MOBA_Self( ent, "^3!pick N^7 takes one of them for you, no captain in between" );
	}
	else if ( assign && ( team == TEAM_RED || team == TEAM_BLUE ) )
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
// Inventory slots
//
// A player owns exactly MOBA_ACTIVE_SLOTS items. Slot 0 is the C key, slot 1 the
// V key, an item lives in exactly one of them and gives its passive bonuses for
// as long as it sits there. A stackable item (the grenade) carries its charges
// inside the slot instead of taking a second one.
//=========================================================================

// the slot that already holds this item, -1 when none of them does
static int MOBA_FindSlotOf( const mobaPlayer_t *p, int id )
{
	int i;

	for ( i = 0; i < MOBA_ACTIVE_SLOTS; i++ )
	{
		if ( p->slotItem[i] == id )
		{
			return i;
		}
	}

	return -1;
}

// the first empty slot, -1 when both are taken
static int MOBA_FreeSlot( const mobaPlayer_t *p )
{
	int i;

	for ( i = 0; i < MOBA_ACTIVE_SLOTS; i++ )
	{
		if ( p->slotItem[i] < 0 )
		{
			return i;
		}
	}

	return -1;
}

// the item mask is only a mirror of the slots, it exists because the stat code
// and the shop window both want "which items are owned" as one bitfield
static void MOBA_SyncItemMask( mobaPlayer_t *p )
{
	int i;

	p->itemMask = 0;

	for ( i = 0; i < MOBA_ACTIVE_SLOTS; i++ )
	{
		if ( p->slotItem[i] >= 0 && p->slotItem[i] < mobaNumItems )
		{
			p->itemMask |= ( 1 << p->slotItem[i] );
		}
	}
}

// Puts one copy of an item into the inventory and takes the gold for it. Every
// reason a purchase can be refused is answered from here, so the shop window and
// the !buyall command refuse a buy the same way the player reads it.
static qboolean MOBA_TakeItem( gentity_t *ent, int id )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	int have, slot;

	if ( id < 0 || id >= mobaNumItems )
	{
		return qfalse;
	}

	have = MOBA_FindSlotOf( p, id );

	if ( have >= 0 && mobaItems[id].maxCount <= 1 )
	{
		MOBA_CPSelf( ent, "You already have %s!\n", mobaItems[id].name );
		return qfalse;
	}

	if ( have >= 0 && p->slotCount[have] >= mobaItems[id].maxCount )
	{
		MOBA_CPSelf( ent, "You already carry the maximum of %i %s!\n",
			mobaItems[id].maxCount, mobaItems[id].name );
		return qfalse;
	}

	if ( have < 0 && MOBA_FreeSlot( p ) < 0 )
	{
		MOBA_CPSelf( ent, "Both item slots (%i and %i) are full!\n",
			MOBA_ITEM_KEY_A, MOBA_ITEM_KEY_B );
		return qfalse;
	}

	if ( p->gold < mobaItems[id].price )
	{
		MOBA_CPSelf( ent, "Not enough gold (%i/%i)!\n", p->gold, mobaItems[id].price );
		return qfalse;
	}

	p->gold -= mobaItems[id].price;

	if ( have >= 0 )
	{
		p->slotCount[have]++;
		MOBA_CPSelf( ent, "%s x%i! %i gold left\n",
			mobaItems[id].name, p->slotCount[have], p->gold );
	}
	else
	{
		slot = MOBA_FreeSlot( p );
		p->slotItem[slot] = id;
		p->slotCount[slot] = 1;
		MOBA_CPSelf( ent, "Bought %s -> slot %c! %i gold left\n",
			mobaItems[id].name, MOBA_ITEM_KEY_A + slot, p->gold );
	}

	MOBA_SyncItemMask( p );
	MOBA_ApplyHeroStats( ent );

	return qtrue;
}

//=========================================================================
// Buys every item the player can still afford, cheapest first, so that a
// single command fills out a round instead of one command per item. It stops
// as soon as both slots are taken, that is the point of the slot limit.
//=========================================================================
static void MOBA_BuyAll( gentity_t *ent )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	int i, best, boughtCount = 0;

	while ( MOBA_FreeSlot( p ) >= 0 )
	{
		best = -1;

		for ( i = 0; i < mobaNumItems; i++ )
		{
			if ( p->gold < mobaItems[i].price )
			{
				continue;
			}

			// a stacking item can go on top of its own slot, everything else
			// only fits into an empty one, which the while loop guarantees
			if ( MOBA_FindSlotOf( p, i ) >= 0 && mobaItems[i].maxCount <= 1 )
			{
				continue;
			}

			if ( best < 0 || mobaItems[i].price < mobaItems[best].price )
			{
				best = i;
			}
		}

		if ( best < 0 )
		{
			break;
		}

		if ( !MOBA_TakeItem( ent, best ) )
		{
			break;
		}

		boughtCount++;
	}

	if ( boughtCount == 0 )
	{
		MOBA_Self( ent, "^3Nothing bought: not enough gold for an empty slot (you have %i gold)", p->gold );
	}
	else
	{
		MOBA_Self( ent, "^2Buyall:^7 %i item(s) bought, ^2%i^7 gold left", boughtCount, p->gold );
	}
}

static void MOBA_BuyItem( gentity_t *ent, int id )
{
	if ( mobaPhase != MOBA_PHASE_BUY )
	{
		MOBA_CPSelf( ent, "Buying is only allowed in the buy phase!\n" );
		return;
	}

	if ( id == -2 )
	{
		MOBA_Self( ent, "^3No item with that code. Codes: armor, med, rage, heavy, "
			"crystal, shadowcloak, grenade, umbrella, inviscloak" );
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

	MOBA_TakeItem( ent, id );
}

//=========================================================================
// Using an item
//
// The key press is answered here, never in the cgame: which slot was pressed,
// what is in it, whether it is on cooldown, whether the key was pressed twice
// and who is standing under the crosshair are all questions only the server can
// answer, and a client that had the answer would simply lie about it.
//=========================================================================
extern gentity_t *WP_DropThermal( gentity_t *ent );

// The ally the crosshair is on, or NULL. The trace runs over the same view the
// client draws, so "aim at a friend" means the same thing on both sides, and a
// client cannot reach further than MOBA_ITEM_MEDKIT_RANGE units.
static gentity_t *MOBA_TraceAlly( gentity_t *ent, float range )
{
	vec3_t start, end, fwd;
	trace_t tr;
	gentity_t *t;

	if ( !ent || !ent->client )
	{
		return NULL;
	}

	AngleVectors( ent->client->ps.viewangles, fwd, NULL, NULL );
	VectorCopy( ent->client->ps.origin, start );
	start[2] += 24.0f;
	VectorMA( start, range, fwd, end );

	trap->Trace( &tr, start, ent->r.mins, ent->r.maxs, end, ent->s.number,
		MASK_SHOT, qfalse, 0, 0 );

	if ( tr.entityNum <= 0 || tr.entityNum >= ENTITYNUM_WORLD )
	{
		return NULL;
	}

	t = &g_entities[ tr.entityNum ];

	if ( !t->inuse || !t->client || t->client->ps.stats[STAT_HEALTH] <= 0 )
	{
		return NULL;
	}

	if ( t == ent )
	{
		return t;
	}

	return MOBA_IsAlly( ent, t ) ? t : NULL;
}

static qboolean MOBA_HealTarget( gentity_t *targ, int amount )
{
	int maxHealth, before;

	if ( !targ || !targ->client )
	{
		return qfalse;
	}

	maxHealth = targ->client->ps.stats[STAT_MAX_HEALTH];

	if ( maxHealth <= 0 )
	{
		maxHealth = MOBA_DEFAULT_MAXHEALTH;
	}

	before = targ->client->ps.stats[STAT_HEALTH];

	if ( before >= maxHealth )
	{
		return qfalse;
	}

	targ->client->ps.stats[STAT_HEALTH] += amount;
	if ( targ->client->ps.stats[STAT_HEALTH] > maxHealth )
	{
		targ->client->ps.stats[STAT_HEALTH] = maxHealth;
	}

	// G_Damage copies the stat back into the body, so a heal that only touches
	// the stat would be gone the next time anything hurts that player
	targ->health = targ->client->ps.stats[STAT_HEALTH];

	return qtrue;
}

static void MOBA_UseMedKit( gentity_t *ent, int slot )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	gentity_t *targ;
	qboolean doublePress;

	// the press that lands inside the double press window asks for the owner
	doublePress = ( level.time - p->slotLastUse[slot] ) <= MOBA_ITEM_DOUBLE_PRESS_MS;
	targ = doublePress ? ent : MOBA_TraceAlly( ent, MOBA_ITEM_MEDKIT_RANGE );

	// nothing to heal: the kit is not spent, but the slot still locks so the
	// key cannot be mashed for a free target scan
	if ( !targ || !MOBA_HealTarget( targ, MOBA_ITEM_MEDKIT_HEAL ) )
	{
		p->slotCdReady[slot] = level.time + MOBA_ITEM_MEDKIT_CD;
		MOBA_CPSelf( ent, doublePress ?
			"Med Kit: you are already at full health!\n" :
			"Med Kit: no wounded ally within %i units!\n", (int)MOBA_ITEM_MEDKIT_RANGE );
		return;
	}

	p->slotCdReady[slot] = level.time + MOBA_ITEM_MEDKIT_CD;

	if ( targ == ent )
	{
		MOBA_CPSelf( ent, "Med Kit: +%i health\n", MOBA_ITEM_MEDKIT_HEAL );
		return;
	}

	MOBA_CPAll( "%s healed %s for %i\n", ent->client->pers.netname,
		targ->client->pers.netname, MOBA_ITEM_MEDKIT_HEAL );
}

static void MOBA_UseGrenade( gentity_t *ent, int slot )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	gentity_t *grenade;

	if ( p->slotCount[slot] <= 0 )
	{
		MOBA_CPSelf( ent, "No %s left in that slot!\n", mobaItems[ MOBA_ITEM_GRENADE ].name );
		return;
	}

	grenade = WP_DropThermal( ent );

	if ( !grenade )
	{
		MOBA_CPSelf( ent, "The %s did not come out!\n", mobaItems[ MOBA_ITEM_GRENADE ].name );
		return;
	}

	p->slotCount[slot]--;
	p->slotCdReady[slot] = level.time + MOBA_ITEM_GRENADE_CD;
}

static void MOBA_UseUmbrella( gentity_t *ent, int slot )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];

	if ( p->mana < MOBA_ITEM_UMBRELLA_MANA )
	{
		MOBA_CPSelf( ent, "Not enough mana for %s (%i needed)\n",
			mobaItems[ MOBA_ITEM_UMBRELLA ].name, MOBA_ITEM_UMBRELLA_MANA );
		return;
	}

	p->mana -= MOBA_ITEM_UMBRELLA_MANA;
	p->umbrellaEndTime = level.time + MOBA_ITEM_UMBRELLA_DUR;

	// the cooldown only starts once the shield is gone, so the slot is locked
	// for the twenty seconds of the shield plus thirty more
	p->slotCdReady[slot] = p->umbrellaEndTime + MOBA_ITEM_UMBRELLA_CD;

	MOBA_ApplyHeroStats( ent );
	MOBA_CPSelf( ent, "Umbrella up: +%i armor for %i sec\n",
		MOBA_ITEM_UMBRELLA_ARMOR, MOBA_ITEM_UMBRELLA_DUR / 1000 );
}

static void MOBA_UseInvisCloak( gentity_t *ent, int slot )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];

	if ( p->mana < MOBA_ITEM_CLOAK_MANA )
	{
		MOBA_CPSelf( ent, "Not enough mana for %s (%i needed)\n",
			mobaItems[ MOBA_ITEM_INVIS_CLOAK ].name, MOBA_ITEM_CLOAK_MANA );
		return;
	}

	p->mana -= MOBA_ITEM_CLOAK_MANA;
	p->invisStartTime = level.time;
	p->invisEndTime = level.time + MOBA_ITEM_CLOAK_DUR;
	p->slotCdReady[slot] = level.time + MOBA_ITEM_CLOAK_CD;

	MOBA_CPSelf( ent, "Cloak on: invisible for %i sec, the next hit breaks it\n",
		MOBA_ITEM_CLOAK_DUR / 1000 );
}

// One press of a slot key. The item has to sit in that slot, be activatable and
// be off cooldown, then the item itself decides what a use means.
static void MOBA_UseItem( gentity_t *ent, int slot )
{
	mobaPlayer_t *p;
	const mobaItem_t *it;
	int id;

	if ( !mobaEnabled || !ent || !ent->client )
	{
		return;
	}

	if ( ent->s.number < 0 || ent->s.number >= MAX_CLIENTS )
	{
		return;
	}

	p = &mobaPlayers[ent->s.number];

	if ( slot < 0 || slot >= MOBA_ACTIVE_SLOTS )
	{
		return;
	}

	if ( mobaPhase != MOBA_PHASE_FIGHT )
	{
		MOBA_CPSelf( ent, "Items only work in the fight phase!\n" );
		return;
	}

	if ( p->heroId < 0 )
	{
		MOBA_CPSelf( ent, "Pick a hero first ( !pick N )!\n" );
		return;
	}

	if ( p->dead || ent->client->ps.pm_type == PM_DEAD )
	{
		MOBA_CPSelf( ent, "You are dead!\n" );
		return;
	}

	id = p->slotItem[slot];

	if ( id < 0 || id >= mobaNumItems )
	{
		MOBA_CPSelf( ent, "Nothing in the %c slot\n", MOBA_ITEM_KEY_A + slot );
		return;
	}

	it = &mobaItems[id];

	if ( it->cooldownMs <= 0 )
	{
		MOBA_CPSelf( ent, "%s has no active use, it only gives its bonus\n", it->name );
		return;
	}

	if ( level.time < p->slotCdReady[slot] )
	{
		MOBA_CPSelf( ent, "%s on cooldown (%i sec)\n",
			it->name, ( p->slotCdReady[slot] - level.time + 999 ) / 1000 );
		return;
	}

	// remembered after the use, a second press inside the window is the double
	// press and means "on me"
	p->slotLastUse[slot] = level.time;

	switch ( id )
	{
	case MOBA_ITEM_MEDKIT:		MOBA_UseMedKit( ent, slot );		break;
	case MOBA_ITEM_GRENADE:		MOBA_UseGrenade( ent, slot );		break;
	case MOBA_ITEM_UMBRELLA:	MOBA_UseUmbrella( ent, slot );		break;
	case MOBA_ITEM_INVIS_CLOAK:	MOBA_UseInvisCloak( ent, slot );	break;
	default:
		MOBA_CPSelf( ent, "%s cannot be used from a slot\n", it->name );
		break;
	}
}

//=========================================================================
// The cloak, per viewer
//
// What one viewer is allowed to see of the cloaked player: nothing, a ghost or
// the whole body. An enemy gets MOBA_INVIS_HIDDEN, everybody who is not an enemy
// (own team, the wearer itself, a spectator) gets the ghost.
static int MOBA_InvisModeFor( int viewerNum, int cloakedNum )
{
	gentity_t *viewer = &g_entities[viewerNum];

	if ( viewerNum == cloakedNum ||
		viewer->client->sess.sessionTeam == TEAM_SPECTATOR ||
		OnSameTeam( viewer, &g_entities[cloakedNum] ) )
	{
		return MOBA_INVIS_GHOST;
	}

	return MOBA_INVIS_HIDDEN;
}

// Every client draws every player, so the cloak has to reach all of them and not
// only the one who wears it. The value only changes once a second while the fade
// runs, so a viewer gets at most one message a second and nothing at all while
// nothing changes.
static void MOBA_PushInvis( int clientNum )
{
	mobaPlayer_t *p = &mobaPlayers[clientNum];
	int left = ( p->invisEndTime > level.time ) ? ( p->invisEndTime - level.time ) : 0;
	int i;

	if ( left <= 0 )
	{
		MOBA_ClearInvis( clientNum );
		return;
	}

	// rounded to a second: the fade is a smooth ramp, the last bit of a
	// millisecond is not worth a message per viewer per frame
	left = ( ( left + 999 ) / 1000 ) * 1000;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		int mode, packed;

		if ( !g_entities[i].client || !g_entities[i].client->pers.connected )
		{
			continue;
		}

		mode = MOBA_InvisModeFor( i, clientNum );

		// the mode rides in the low bits, so a viewer that changes sides stops
		// getting the old value even though the time did not move
		packed = ( left << 2 ) | mode;

		if ( packed == mobaInvisSent[clientNum][i] )
		{
			continue;
		}

		mobaInvisSent[clientNum][i] = packed;
		trap->SendServerCommand( i, va( "mobaInvis %i %i %i", clientNum, left, mode ) );
	}
}

// The cloak stopped: everybody who was told something is told that it stopped,
// so no client is left with a body it still draws as a ghost.
static void MOBA_ClearInvis( int clientNum )
{
	int i;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
	{
		if ( mobaInvisSent[clientNum][i] == 0 )
		{
			continue;
		}

		mobaInvisSent[clientNum][i] = 0;

		if ( g_entities[i].client )
		{
			trap->SendServerCommand( i, va( "mobaInvis %i 0 %i",
				clientNum, MOBA_INVIS_NONE ) );
		}
	}
}

//=========================================================================
// Item timers
//=========================================================================
static void MOBA_TickItems( int clientNum )
{
	mobaPlayer_t *p = &mobaPlayers[clientNum];
	gentity_t *ent = &g_entities[clientNum];

	if ( !p->inuse || !ent->client )
	{
		return;
	}

	// the shield is a fixed armor pool, so it has to be taken off the stat again
	if ( p->umbrellaEndTime > 0 )
	{
		if ( p->umbrellaEndTime > level.time )
		{
			MOBA_PushInvis( clientNum );
		}
		else
		{
			p->umbrellaEndTime = 0;
			MOBA_ApplyHeroStats( ent );
		}
	}

	if ( p->invisEndTime > 0 )
	{
		if ( p->invisEndTime > level.time )
		{
			MOBA_PushInvis( clientNum );
		}
		else
		{
			p->invisEndTime = 0;
			p->invisStartTime = 0;
			MOBA_ClearInvis( clientNum );
		}
	}
}

// A hit breaks the cloak. Called from the damage path with the attacker as the
// player, so a saber, an ability and a grenade all count as "any hit".
static void MOBA_BreakInvis( gentity_t *attacker )
{
	mobaPlayer_t *p;

	if ( !mobaEnabled || !attacker || !attacker->client ||
		attacker->s.number < 0 || attacker->s.number >= MAX_CLIENTS )
	{
		return;
	}

	p = &mobaPlayers[attacker->s.number];

	if ( p->invisEndTime <= level.time )
	{
		return;
	}

	p->invisEndTime = 0;
	p->invisStartTime = 0;
	MOBA_ClearInvis( attacker->s.number );
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

	if ( !p->dead )
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
	ent->client->tempSpectate = 0;
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

// Hands a hero to a player and applies the model, the shared tail of the assign
// stage and of all pick.
static void MOBA_GiveHero( gentity_t *ent, int heroId )
{
	mobaPlayer_t *p = &mobaPlayers[ent->s.number];
	int i;

	p->heroId = heroId;

	// a fresh hero starts with a clean slate: no leftover shield, silence, slow
	// or channel can survive a hero swap, only the fight state does
	p->shieldEndTime = 0;
	p->shieldAmount = 0;
	p->silenceEndTime = 0;
	p->magicResistEndTime = 0;
	p->slowEndTime = 0;
	p->slowPct = 0.0f;
	p->flameUntil = 0;
	p->flameSlot = -1;
	p->flameTick = 0;
	p->flameDmgAcc = 0;
	p->flameManaFrac = 0;

	for ( i = 0; i < MOBA_ABILITIES_PER_HERO; i++ )
	{
		p->effectEndTime[i] = 0;

		// only the slow travelling bolt is a charged ability; everything else
		// uses its cooldown the plain way and carries -1 charges
		p->charges[i] = ( mobaHeroes[heroId].abilities[i].type == AB_PROJECTILE ) ?
			MOBA_PROJECTILE_CHARGES : -1;
	}

	MOBA_ApplyHeroModel( ent );
	MOBA_CPSelf( ent, "Hero picked: ^5%s^7!\n", mobaHeroes[heroId].name );
	MOBA_LogLine( va( "%s plays %s", ent->client->pers.netname,
		mobaHeroes[heroId].name ), ent );
}

static void MOBA_DraftPick( gentity_t *ent, const char *arg )
{
	int heroId;

	if ( mobaPhase != MOBA_PHASE_DRAFT && mobaPhase != MOBA_PHASE_DRAFT_ASSIGN )
	{
		MOBA_CPSelf( ent, "Heroes can only be picked in the draft!\n" );
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

	// All pick: no captain job, no ban step and no team pool. Every player takes
	// one hero off the shared board, whoever is first owns it, and the hero
	// leaves the board for everybody else as soon as it is owned.
	if ( mobaPhase == MOBA_PHASE_DRAFT && MOBA_ModeAllPick() )
	{
		team_t team = ent->client->sess.sessionTeam;

		// A client that is not on a team yet - a spectator, or one that has just
		// connected and is not in the team for a frame - is shown the board, but
		// it may not take a hero off it: it has no side in the match to play it.
		if ( team != TEAM_RED && team != TEAM_BLUE )
		{
			MOBA_CPSelf( ent, "You are not playing this match yet - wait for the teams.\n" );
			return;
		}

		if ( mobaHeroBanned[heroId] || mobaHeroTeam[heroId] != TEAM_FREE ||
			MOBA_HeroTaken( heroId ) )
		{
			MOBA_CPSelf( ent, "%s is already taken! See: !heroes\n",
				mobaHeroes[heroId].name );
			return;
		}

		MOBA_GiveHero( ent, heroId );
		MOBA_CPAll( "^2PICK^7 - %s takes ^5%s^7\n", ent->client->pers.netname,
			mobaHeroes[heroId].name );

		if ( MOBA_EveryoneHasHero() )
		{
			MOBA_CPAll( "^2Lineup complete^7 - to the shop\n" );
		}
		return;
	}

	if ( mobaPhase == MOBA_PHASE_DRAFT && !MOBA_IsActingCaptain( ent ) )
	{
		MOBA_CPSelf( ent, "Not your turn - only the captains ban and pick.\n" );
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

	MOBA_GiveHero( ent, heroId );

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

	// all pick is the mode without a ban step, a !ban there is a mistake and not
	// a wrong turn
	if ( MOBA_ModeAllPick() )
	{
		MOBA_CPSelf( ent, "All pick has no bans - take any hero with ^3!pick N^7\n" );
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
			"!buyback !1-!%i (abilities) !use1 !use2 (items) !status\n"
			"^3Draft (%s):^7 %s^7, ^3!draft^7 brings the hero window back\n"
			"^3The shop is a window:^7 press ^3B^7 in the buy phase, pick a tab "
			"(^3Defence^7, ^3Attack^7, ^3Consumables^7) and buy with a left click, "
			"^3B^7 or ^3ESC^7 closes it. You own ^3two^7 items, they sit in the "
			"^3%c^7 and ^3%c^7 slots, press the slot key to use one, or press it "
			"twice to use it on yourself",
			MOBA_ABILITIES_PER_HERO, MOBA_ModeName(),
			MOBA_ModeAllPick() ?
				"no bans and no captains, every player takes any hero he likes with "
				"!pick N or a click in the window" :
				"the two captains get !ban N and !pick N, every captain picks one "
				"hero per player of his team and everybody takes one hero out of "
				"the pool of his team afterwards",
			MOBA_ITEM_KEY_A, MOBA_ITEM_KEY_B );
		return qtrue;
	}
	if ( !Q_stricmp( cmd, "!use1" ) )
	{
		MOBA_UseItem( ent, 0 );
		return qtrue;
	}
	if ( !Q_stricmp( cmd, "!use2" ) )
	{
		MOBA_UseItem( ent, 1 );
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
	if ( !Q_stricmp( cmd, "!ready" ) )
	{
		int num = ent->s.number;

		if ( mobaPhase != MOBA_PHASE_LOBBY )
		{
			MOBA_Self( ent, "^3There is no lobby waiting right now^7 - "
				"!ready only works before the draft\n" );
			return qtrue;
		}
		if ( num < 0 || num >= MAX_CLIENTS || ( ent->r.svFlags & SVF_BOT ) )
		{
			return qtrue;
		}
		if ( mobaReady[num] )
		{
			MOBA_Self( ent, "^3You are already ready.\n" );
			return qtrue;
		}
		mobaReady[num] = qtrue;
		MOBA_CPAll( "^2%s is ready!^7 (%i/%i)\n", ent->client->pers.netname,
			MOBA_ReadyCount(), MOBA_ReadyNeeded() );
		if ( MOBA_AllReady() )
		{
			MOBA_CPAll( "^2Everybody is here - the draft starts!\n" );
		}
		return qtrue;
	}
	if ( !Q_stricmp( cmd, "!restart" ) )
	{
		int num = ent->s.number, i, votes = 0, humans = 0, needed;

		if ( num < 0 || num >= MAX_CLIENTS || ( ent->r.svFlags & SVF_BOT ) )
		{
			return qtrue;
		}

		// a new vote opens a 60 second window
		if ( mobaRestartVoteStart == 0 ||
			level.time > mobaRestartVoteStart + 60000 )
		{
			mobaRestartVoteStart = level.time;
			memset( mobaRestartVoted, 0, sizeof( mobaRestartVoted ) );
		}
		if ( mobaRestartVoted[num] )
		{
			MOBA_Self( ent, "^3You already voted for a restart (60s window).\n" );
			return qtrue;
		}
		mobaRestartVoted[num] = qtrue;

		// more than half of the humans on both teams, bots never vote
		for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ )
		{
			if ( !MOBA_SlotActive( i ) || ( g_entities[i].r.svFlags & SVF_BOT ) )
			{
				continue;
			}
			humans++;
			if ( mobaRestartVoted[i] )
			{
				votes++;
			}
		}
		needed = humans / 2 + 1;

		MOBA_CPAll( "^3Restart vote:^7 %i/%i within a minute\n", votes, needed );
		if ( votes >= needed )
		{
			MOBA_CPAll( "^3Majority agrees - restarting the match!\n" );
			MOBA_RestartMatch();
		}
		return qtrue;
	}
	if ( !Q_stricmp( cmd, "!ff" ) )
	{
		int num = ent->s.number;
		team_t team;
		const char *teamName;

		if ( num < 0 || num >= MAX_CLIENTS || ( ent->r.svFlags & SVF_BOT ) )
		{
			return qtrue;
		}
		team = ent->client->sess.sessionTeam;
		if ( team != TEAM_RED && team != TEAM_BLUE )
		{
			MOBA_Self( ent, "^3!ff works only while your side is in the match.\n" );
			return qtrue;
		}
		teamName = ( team == TEAM_RED ) ? "red" : "blue";

		// one minute for the whole team to agree
		if ( mobaFFVoteStart == 0 ||
			level.time > mobaFFVoteStart + 60000 )
		{
			mobaFFVoteStart = level.time;
			memset( mobaFFVoted, 0, sizeof( mobaFFVoted ) );
		}
		if ( mobaFFVoted[num] )
		{
			MOBA_Self( ent, "^3You already said !ff (60s window).\n" );
			return qtrue;
		}
		mobaFFVoted[num] = qtrue;

		MOBA_CPAll( "^1%s !ff^7 - the whole %s team must agree within a minute\n",
			ent->client->pers.netname, teamName );

		if ( MOBA_TeamFFComplete( team ) )
		{
			MOBA_CPAll( "^1Team %s concedes!^7 %s takes the match.\n", teamName,
				( team == TEAM_RED ) ? "blue" : "red" );
			MOBA_RestartMatch();
		}
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
