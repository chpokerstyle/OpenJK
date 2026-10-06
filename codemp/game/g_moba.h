/*
===========================================================================
MagicWands MOBA mod - OpenJK game code (server-side)
===========================================================================
*/

#ifndef G_MOBA_H
#define G_MOBA_H

#include "moba_content.h"

#define MOBA_MAX_ITEMS			9
#define MOBA_MAX_SKILL_LEVEL	5

// ---- Captain draft ----
// The draft runs once per match: three stages, each one bans
// MOBA_DRAFT_BANS_PER_CAPTAIN heroes per captain and then picks
// MOBA_DRAFT_PICKS_PER_STAGE per captain. A captain picks one hero per player
// on his team, so a captain of a five player team ends up with five heroes, and
// a team that grows while the draft runs gets the missing picks appended to the
// plan. All pick skips this plan entirely: no bans, no captain, every player
// takes one hero off the same board.
#define MOBA_DRAFT_STAGES			3
#define MOBA_DRAFT_BANS_PER_CAPTAIN	2
#define MOBA_DRAFT_PICKS_PER_STAGE	2
#define MOBA_DRAFT_ACTIONS_MAX		( MOBA_DRAFT_STAGES * ( MOBA_DRAFT_BANS_PER_CAPTAIN + MOBA_DRAFT_PICKS_PER_STAGE ) * 2 )

// ---- Phases of a MOBA round ----
// cg_moba.c mirrors the numbering of this enum, keep both in sync.
typedef enum {
	MOBA_PHASE_LOBBY = 0,
	MOBA_PHASE_DRAFT,			// captains ban and pick
	MOBA_PHASE_DRAFT_ASSIGN,	// every player takes a hero out of his team pool
	MOBA_PHASE_BUY,				// pre-round shop
	MOBA_PHASE_FIGHT,			// combat round
	MOBA_PHASE_ROUNDEND			// brief intermission between rounds
} mobaPhase_t;

// ---- Ability behaviour, hero table and mobaHero_t live in moba_content.h,
// both the server and the cgame include that one copy ----

// ---- Shop items ----
// Everything above the price is a passive bonus that applies while the item sits
// in one of the two slots. The lower half is only read for the items that can be
// used: a nonzero cooldownMs is what makes an item activatable, maxCount is how
// many charges one slot holds and durationMs how long a use keeps something going.
typedef struct {
	const char		*name;
	const char		*code;			// short alias for "!buy <code>"
	const char		*category;
	int				price;
	int				armorBonus;
	int				healthBonus;
	int				dmgBonusPercent;
	const char		*desc;
	int				maxCount;		// charges one slot holds, 1 = single
	int				cooldownMs;		// between two uses, 0 = passive
	int				manaCost;		// paid on every use
	int				effectAmount;	// heal per use, armor while active
	int				durationMs;		// how long one use keeps an effect
} mobaItem_t;

// ---- Per-player persistent state (survives death, index = clientNum) ----
typedef struct {
	qboolean			inuse;
	int					heroId;			// index into mobaHeroes, -1 = not picked
	int					level;
	int					xp;
	int					gold;
	int					skillPoints;
	int					abilityLevel[MOBA_ABILITIES_PER_HERO];
	int					itemMask;		// bit per owned item
	int					slotItem[MOBA_ACTIVE_SLOTS];		// item id in the slot, -1 = empty
	int					slotCount[MOBA_ACTIVE_SLOTS];		// charges left in that slot
	int					slotCdReady[MOBA_ACTIVE_SLOTS];	// absolute level.time the slot may be used again
	int					slotLastUse[MOBA_ACTIVE_SLOTS];	// absolute level.time of the last use press, for the double press
	int					cdReady[MOBA_ABILITIES_PER_HERO];	// absolute level.time when ability is ready
	int					mana;		// current mana, drained by abilities, regens slowly
	int					manaFrac;	// fractional part of the regen, kept so a 50ms tick can add to 10/sec
	int					lastManaTick;	// level.time of the last regen step
	int					autoCmdNext;	// dev aid: level.time of the next moba_auto replay
	int					autoCmdIdx;	// dev aid: cursor into the moba_auto command list
	qboolean			gotStartGold;	// starting gold is granted once per match
	qboolean			greeted;		// command hint was already sent on spawn
	int					buffEndTime;	// absolute level.time when dmg buff ends
	float				dmgMult;		// permanent damage multiplier from items
	float				buffMult;		// temporary damage multiplier while buffed
	int					effectEndTime[MOBA_ABILITIES_PER_HERO];	// absolute level.time when each ability effect ends, 0 = none
	int					shieldEndTime;	// absolute level.time when the shield expires
	int					shieldAmount;	// remaining shield pool, absorbs damage before health
	int					silenceEndTime;	// absolute level.time until the player may not cast
	int					umbrellaEndTime;	// absolute level.time the +50 armor of the umbrella lasts
	int					invisEndTime;		// absolute level.time the cloak stops working
	int					invisStartTime;	// absolute level.time the cloak was put on, the fade in counts from here
	int					magicResistEndTime;	// absolute level.time of the magic damage reduction buff
	int					slowEndTime;	// absolute level.time until the movement slow expires
	float				slowPct;		// movement slow fraction, 0.10 = 10% (strongest active slow wins)
	int					charges[MOBA_ABILITIES_PER_HERO];	// ability charges, -1 = not a charged ability
	int					flameUntil;		// absolute level.time until the held flame stays lit
	int					flameSlot;		// ability slot the flame channel belongs to
	int					flameTick;		// level.time of the last flame damage tick
	int					flameDmgAcc;	// fractional flame damage, kept so 100ms ticks add to per-second damage
	int					flameManaFrac;	// fractional flame mana cost
	int					flameFxTime;	// level.time of the last flame puff along the cone
	int					flameSndTime;	// level.time of the last flame sound
	int					roundKills;
	int					kills;			// whole-match kills, reset with the match
	int					deaths;			// whole-match deaths, reset with the match
	qboolean			dead;			// true if killed during FIGHT and waiting for buyback
} mobaPlayer_t;

extern mobaHero_t		mobaHeroes[MOBA_MAX_HEROES];
extern int				mobaNumHeroes;
extern mobaItem_t		mobaItems[MOBA_MAX_ITEMS];
extern int				mobaNumItems;
extern mobaPlayer_t		mobaPlayers[MAX_CLIENTS];

// ---- Core API (called from the vanilla OpenJK files) ----
void					MOBA_InitGame( void );			// g_main: G_InitGame
void					MOBA_RunFrame( void );			// g_main: G_RunFrame (once per frame)
qboolean				MOBA_Active( void );			// g_moba cvar is 1
mobaPhase_t				MOBA_GetPhase( void );		// g_active: ClientThink blocks attacks while a draft runs
qboolean				MOBA_CanSuicide( gentity_t *ent );	// g_cmds: G_Kill
qboolean				MOBA_ShouldBlockDamage( gentity_t *targ, gentity_t *attacker );
int						MOBA_AdjustDamage( gentity_t *targ, gentity_t *attacker, gentity_t *inflictor, int meansOfDeath, int damage );
void					MOBA_OnPlayerDeath( gentity_t *self, gentity_t *attacker, int meansOfDeath );
void					MOBA_ClientThink( gentity_t *ent );	// g_active: ClientThink, applies the movement slow
void					MOBA_OnMissileImpact( gentity_t *missile, gentity_t *other );	// g_missile: fireball slow on hit
void					MOBA_OnClientSpawn( gentity_t *ent );
void					MOBA_OnClientDisconnect( gentity_t *ent );
qboolean				MOBA_HandleChat( gentity_t *ent, const char *msg );	// commands starting with '!'
gentity_t				*MOBA_PickSpawnPoint( gentity_t *ent, vec3_t origin, vec3_t angles );

#endif // G_MOBA_H