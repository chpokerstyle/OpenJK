/*
===========================================================================
MagicWands MOBA mod - OpenJK game code (server-side)
===========================================================================
*/

#ifndef G_MOBA_H
#define G_MOBA_H

#define MOBA_MAX_HEROES			30
#define MOBA_ABILITIES_PER_HERO	4
#define MOBA_MAX_ITEMS			6
#define MOBA_MAX_SKILL_LEVEL	5

// ---- Phases of a MOBA round ----
typedef enum {
	MOBA_PHASE_LOBBY = 0,
	MOBA_PHASE_DRAFT,			// hero pick
	MOBA_PHASE_BUY,				// pre-round shop
	MOBA_PHASE_FIGHT,			// combat round
	MOBA_PHASE_ROUNDEND			// brief intermission between rounds
} mobaPhase_t;

// ---- Ability behaviour ----
typedef enum {
	AB_DIRECT		= 0,	// raycast under crosshair, damage single enemy
	AB_AOE_DAMAGE	= 1,	// damage all enemies in a radius around caster
	AB_AOE_HEAL		= 2,	// heal all allies (incl. self) in a radius
	AB_BUFF			= 3		// self buff: damage multiplier for duration
} mobaAbilityType_t;

typedef struct {
	const char		*name;
	int				type;
	int				cooldownMs;
	int				baseEffect;		// base damage / heal
	int				perLevelEffect;
	float			range;			// for AB_DIRECT
	float			radius;			// for AoE
	int				durationMs;		// for AB_BUFF
	float			buffMult;		// multiplier for AB_BUFF
	const char		*desc;
} mobaAbility_t;

typedef struct {
	const char		*name;
	const char		*role;
	int				baseHealth;
	int				healthPerLevel;
	int				baseArmor;
	int				baseDamage;		// reserved for auto-attack scaling
	int				damagePerLevel;
	mobaAbility_t	abilities[MOBA_ABILITIES_PER_HERO];
} mobaHero_t;

// ---- Shop items ----
typedef struct {
	const char		*name;
	const char		*code;			// short alias for "!buy <code>"
	const char		*category;
	int				price;
	int				armorBonus;
	int				healthBonus;
	int				dmgBonusPercent;
	const char		*desc;
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
	int					cdReady[MOBA_ABILITIES_PER_HERO];	// absolute level.time when ability is ready
	int					autoCmdNext;	// dev aid: level.time of the next moba_auto replay
	int					autoCmdIdx;	// dev aid: cursor into the moba_auto command list
	qboolean			gotStartGold;	// starting gold is granted once per match
	qboolean			greeted;		// command hint was already sent on spawn
	int					buffEndTime;	// absolute level.time when dmg buff ends
	float				dmgMult;		// permanent damage multiplier from items
	float				buffMult;		// temporary damage multiplier while buffed
	int					roundKills;
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
qboolean				MOBA_CanSuicide( gentity_t *ent );	// g_cmds: G_Kill
qboolean				MOBA_ShouldBlockDamage( gentity_t *targ, gentity_t *attacker );
int						MOBA_AdjustDamage( gentity_t *targ, gentity_t *attacker, int damage );
void					MOBA_OnPlayerDeath( gentity_t *self, gentity_t *attacker, int meansOfDeath );
void					MOBA_OnClientSpawn( gentity_t *ent );
void					MOBA_OnClientDisconnect( gentity_t *ent );
qboolean				MOBA_HandleChat( gentity_t *ent, const char *msg );	// commands starting with '!'
gentity_t				*MOBA_PickSpawnPoint( gentity_t *ent, vec3_t origin, vec3_t angles );

#endif // G_MOBA_H