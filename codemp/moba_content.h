/*
===========================================================================
MagicWands MOBA mod - content shared by the game and the cgame
===========================================================================
The hero table is content, not state: the server owns the authoritative copy
in mobaHeroes[] while the cgame has to render the very same numbers, names and
ability values in the hero select window. Two hand written copies of the hero
table would drift apart within a day, so the table itself lives here and both
modules include it.

The table is static, so every including translation unit gets its own copy and
no symbol is exported. That is a few kilobytes of read only data in exchange
for a build system that does not have to know about a new module.

The "weapons" column is the loadout a hero is built around. It is shown in the
info panel and is not enforced by the server yet: MOBA_ApplyHeroStats only
touches health and armor, every hero still spawns with the vanilla saber and
force setup. Restricting weapons per hero is a balance change and has to be
decided on its own.
===========================================================================
*/

#ifndef MOBA_CONTENT_H
#define MOBA_CONTENT_H

#define MOBA_MAX_HEROES			33
#define MOBA_ABILITIES_PER_HERO	2

// ---- Mana economy (shared by the server and the client HUD bar) ----
#define MOBA_MANA_REGEN_PER_SEC		10	// slow regen, always on during a fight

// ---- Active items (shared by the server rules and the client slot bar) ----
// Two item slots per player: C is the first one, V the second one. A slot holds
// one item, a stackable one (the grenade) carries its charges inside the slot.
#define MOBA_ACTIVE_SLOTS		2
#define MOBA_ITEM_KEY_A			'C'
#define MOBA_ITEM_KEY_B			'V'

// ---- Item ids. The order is the order of mobaItems[] on the server, the bit
// order of the item mask and the id the server pushes for a slot, so the cgame
// mirror of that table can never be sorted apart from it. ----
typedef enum {
	MOBA_ITEM_STURDY_ARMOR	= 0,
	MOBA_ITEM_MEDKIT		= 1,
	MOBA_ITEM_RAGE_RUNE		= 2,
	MOBA_ITEM_HEAVY_PLATE	= 3,
	MOBA_ITEM_POWER_CRYSTAL	= 4,
	MOBA_ITEM_SHADOW_CLOAK	= 5,
	MOBA_ITEM_GRENADE		= 6,
	MOBA_ITEM_UMBRELLA		= 7,
	MOBA_ITEM_INVIS_CLOAK	= 8
} mobaItemId_t;

#define MOBA_NUM_ITEM_IDS	9

#define MOBA_ITEM_MEDKIT_RANGE		100.0f	// ally must be this close to the crosshair
#define MOBA_ITEM_MEDKIT_HEAL		100
#define MOBA_ITEM_MEDKIT_CD		3000

#define MOBA_ITEM_GRENADE_MAX		4
#define MOBA_ITEM_GRENADE_CD		3000

#define MOBA_ITEM_UMBRELLA_ARMOR	50
#define MOBA_ITEM_UMBRELLA_MANA		50
#define MOBA_ITEM_UMBRELLA_DUR		20000
#define MOBA_ITEM_UMBRELLA_CD		30000	// starts when the shield is gone

#define MOBA_ITEM_CLOAK_MANA		60
#define MOBA_ITEM_CLOAK_FADE		1500	// the fade in takes this long
#define MOBA_ITEM_CLOAK_DUR		8000	// total, the fade in included
#define MOBA_ITEM_CLOAK_CD		40000

// Who sees what under a cloak. The server works this out per viewer, so an
// enemy cannot see the wearer at all while the wearer stays a visible ghost for
// their own team and for the spectators, which is what makes the cloak usable
// for the team instead of only as a way to cheat a duel.
#define MOBA_INVIS_NONE		0	// nothing is cloaked, the body is drawn as it is
#define MOBA_INVIS_HIDDEN	1	// an enemy: fully invisible
#define MOBA_INVIS_GHOST	2	// an ally, the wearer itself or a spectator: see through
#define MOBA_INVIS_GHOST_ALPHA	110	// how see-through a cloak looks to those, out of 255

// A second press of the same slot key inside this window uses the item on the
// owner instead of on whoever is under the crosshair.
#define MOBA_ITEM_DOUBLE_PRESS_MS	350

// ---- Ability behaviour ----
typedef enum {
	AB_DIRECT		= 0,	// raycast under crosshair, damage single enemy
	AB_AOE_DAMAGE	= 1,	// damage all enemies in a radius around caster
	AB_AOE_HEAL		= 2,	// heal all allies (incl. self) in a radius
	AB_BUFF			= 3,	// self buff: damage multiplier for duration
	AB_LEAP			= 4,	// jump forward, knock down and damage on landing
	AB_SHIELD		= 5,	// self shield: absorb a pool of incoming damage
	AB_PROJECTILE	= 6,	// slow travelling bolt, damage + slow on hit
	AB_FLAME		= 7,	// held channel: damage and mana drain over time
	AB_SILENCE		= 8,	// block enemy abilities in a radius
	AB_MAGICRESIST	= 9		// ally buff: incoming magic damage reduced
} mobaAbilityType_t;

typedef struct {
	const char		*name;
	int				type;
	int				cooldownMs;
	int				manaCost;		// mana spent when the ability is cast
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
	const char		*weapons;		// loadout flavour, shown in the info panel
	const char		*model;		// models/players/<model>, the body the player spawns as
	int				maxMana;		// mana pool, sized by role on purpose
	int				baseHealth;
	int				healthPerLevel;
	int				baseArmor;
	int				baseDamage;		// reserved for auto-attack scaling
	int				damagePerLevel;
	mobaAbility_t	abilities[MOBA_ABILITIES_PER_HERO];
} mobaHero_t;

#define MOBA_AB(n,t,cd,mc,d,pl,r,rad,dur,mul,desc) { n, t, cd, mc, d, pl, r, rad, dur, mul, desc }

static const mobaHero_t mobaHeroTable[MOBA_MAX_HEROES] = {
	{ "Ash'Lar",		"Tank",		"Lightsaber",		"kyle",		150, 950, 50, 100, 30, 2,
	{ MOBA_AB( "Piercing Blade",	AB_DIRECT,		6000,	20,	90,	20,	900,	0,	0,	1.0f, "Hit target" ),
	  MOBA_AB( "Blade Wall",		AB_AOE_DAMAGE,	12000,	45,	70,	15,	0,		450,	0,	1.0f, "Sweeping strike" ) } },

	{ "Vectar",		"Tank",		"War hammer",		"lando",		150, 920, 48, 100, 28, 2,
	{ MOBA_AB( "Thunder Hammer",		AB_DIRECT,		7000,	25,	95,	18,	800,	0,	0,	1.0f, "Stunning blow" ),
	  MOBA_AB( "Stone Skin",		AB_BUFF,		18000,	40,	0,	0,	0,		0,		10000,	1.35f, "+35% dmg 10s" ) } },

	{ "T'Raine",		"Tank",		"Short sword",		"imperial",		150, 980, 55, 100, 26, 2,
	{ MOBA_AB( "Spike",				AB_DIRECT,		6500,	20,	85,	16,	850,	0,	0,	1.0f, "Piercing thrust" ),
	  MOBA_AB( "Iron Ring",			AB_AOE_DAMAGE,	12000,	45,	65,	14,	0,		450,	0,	1.0f, "Ring of damage" ) } },

	{ "Korkhin",		"Mage",		"Saber + Force",		"jedi",	300, 560, 28, 50, 22, 2,
	{ MOBA_AB( "Lightning Lash",		AB_DIRECT,		6000,	30,	110,	25,	1000,	0,	0,	1.0f, "Lightning at target" ),
	  MOBA_AB( "Fireball",			AB_AOE_DAMAGE,	10000,	50,	85,	18,	0,		400,	0,	1.0f, "Explosion around self" ) } },

	{ "Silvara",		"Mage",		"Saber + Force",		"chiss",	300, 540, 26, 50, 20, 2,
	{ MOBA_AB( "Ice Dagger",		AB_DIRECT,		5500,	25,	105,	24,	1000,	0,	0,	1.0f, "Ice at target" ),
	  MOBA_AB( "Frost Breath",		AB_BUFF,		16000,	40,	0,	0,	0,		0,		7000,	1.4f, "+40% dmg 7s" ) } },

	{ "Merek",		"Mage",		"Saber + Force",		"jeditrainer",	320, 580, 30, 55, 24, 2,
	{ MOBA_AB( "Spirit Fire",		AB_DIRECT,		6500,	35,	115,	20,	950,	0,	0,	1.0f, "Flaming beam" ),
	  MOBA_AB( "Fire Cocktail",		AB_AOE_DAMAGE,	10000,	50,	80,	20,	0,		420,	0,	1.0f, "Explosion" ) } },

	{ "Ornat",		"Mage",		"Saber + Force",		"tavion",	320, 520, 25, 50, 22, 2,
	{ MOBA_AB( "Acid Shot",		AB_DIRECT,		6000,	35,	120,	28,	1000,	0,	0,	1.0f, "Acid" ),
	  MOBA_AB( "Rot Wave",			AB_AOE_DAMAGE,	11000,	50,	70,	15,	0,		430,	0,	1.0f, "Rot around" ) } },

	{ "Zum'Zar",		"Mage",		"Saber + Force",		"tavion_new",	300, 550, 27, 55, 22, 2,
	{ MOBA_AB( "Thunder Strike",	AB_DIRECT,		7000,	40,	130,	26,	900,	0,	0,	1.0f, "Thunder" ),
	  MOBA_AB( "Thunderclap",		AB_AOE_DAMAGE,	11000,	50,	78,	17,	0,		480,	0,	1.0f, "Shock wave" ) } },

	{ "Killian",		"Carry",	"Blaster rifle",	"rockettrooper",	200, 620, 32, 60, 35, 3,
	{ MOBA_AB( "Precision Shot",		AB_DIRECT,		5000,	25,	100,	22,	1100,	0,	0,	1.0f, "Shot" ),
	  MOBA_AB( "Rapid Fire",		AB_BUFF,		14000,	35,	0,	0,	0,		0,		6000,	1.5f, "+50% dmg 6s" ) } },

	{ "Dinara",		"Carry",	"Twin sabers",	"shadowtrooper",		200, 600, 30, 65, 38, 3,
	{ MOBA_AB( "Twin Blades",		AB_DIRECT,		5500,	25,	95,	20,	1000,	0,	0,	1.0f, "Double strike" ),
	  MOBA_AB( "Blade Dance",		AB_AOE_DAMAGE,	12000,	50,	70,	15,	0,		420,	0,	1.0f, "Ring of blades" ) } },

	{ "Starr",		"Carry",	"Heavy blaster",	"snowtrooper",	200, 640, 34, 70, 36, 3,
	{ MOBA_AB( "Assault Volley",		AB_DIRECT,		5000,	20,	85,	18,	1050,	0,	0,	1.0f, "Volley" ),
	  MOBA_AB( "Roaring Barrage",	AB_AOE_DAMAGE,	11000,	45,	60,	15,	0,		400,	0,	1.0f, "Wave" ) } },

	{ "Brock",		"Carry",	"War axe",	"hazardtrooper",			200, 660, 35, 70, 34, 3,
	{ MOBA_AB( "Chopping Blow",		AB_DIRECT,		6000,	25,	90,	19,	950,	0,	0,	1.0f, "Axe" ),
	  MOBA_AB( "Whirl",				AB_AOE_DAMAGE,	11000,	45,	65,	14,	0,		420,	0,	1.0f, "Axe whirlwind" ) } },

	{ "Lira",		"Healer",	"Lightsaber",	"monmothma",		260, 600, 30, 60, 22, 1,
	{ MOBA_AB( "Light Discipline",	AB_AOE_HEAL,	6000,	30,	80,	15,	0,		600,	0,	1.0f, "Area heal" ),
	  MOBA_AB( "Ray of Hope",		AB_DIRECT,		8000,	25,	70,	14,	950,	0,	0,	1.0f, "Beam" ) } },

	{ "Selena",		"Healer",	"Light spear",	"jan",		260, 580, 28, 55, 20, 1,
	{ MOBA_AB( "Wave of Life",		AB_AOE_HEAL,	6500,	30,	75,	14,	0,		550,	0,	1.0f, "Heal" ),
	  MOBA_AB( "Light Spear",		AB_DIRECT,		7000,	25,	80,	16,	1000,	0,	0,	1.0f, "Spear" ) } },

	{ "Mornan",		"Healer",	"War hammer",	"gran",		260, 640, 32, 65, 24, 1,
	{ MOBA_AB( "Balm",				AB_AOE_HEAL,	6000,	35,	85,	16,	0,		580,	0,	1.0f, "Heal" ),
	  MOBA_AB( "Hammer of Fate",		AB_DIRECT,		8500,	30,	90,	18,	900,	0,	0,	1.0f, "Hammer" ) } },

	{ "Gillian",		"Assassin",	"Twin sabers",	"alora",		180, 540, 26, 45, 40, 4,
	{ MOBA_AB( "Shadow Stab",		AB_DIRECT,		4500,	30,	130,	30,	1000,	0,	0,	1.0f, "Stab" ),
	  MOBA_AB( "Shadow Blades",		AB_AOE_DAMAGE,	10000,	45,	80,	18,	0,		400,	0,	1.0f, "Blade circles" ) } },

	{ "Kyra",		"Assassin",	"Saber + Force",	"alora2",	180, 520, 24, 45, 42, 4,
	{ MOBA_AB( "Backstab",			AB_DIRECT,		5000,	25,	120,	28,	950,	0,	0,	1.0f, "Dagger" ),
	  MOBA_AB( "Blood Dance",		AB_AOE_DAMAGE,	10000,	45,	75,	16,	0,		380,	0,	1.0f, "Dance" ) } },

	{ "Ravel",		"Assassin",	"Twin sabers",	"reelo",		180, 560, 27, 50, 38, 4,
	{ MOBA_AB( "Knife Whirl",		AB_DIRECT,		4800,	25,	110,	26,	1050,	0,	0,	1.0f, "Whirl" ),
	  MOBA_AB( "Wind Blades",		AB_AOE_DAMAGE,	10000,	45,	70,	15,	0,		420,	0,	1.0f, "Blades" ) } },

	{ "Ismara",		"Carry",	"Blaster rifle",	"rebel_pilot",	200, 600, 31, 65, 37, 3,
	{ MOBA_AB( "Fire Volley",		AB_DIRECT,		5500,	30,	105,	24,	1050,	0,	0,	1.0f, "Volley" ),
	  MOBA_AB( "Burst Fire",		AB_AOE_DAMAGE,	10000,	45,	70,	15,	0,		400,	0,	1.0f, "Barrage" ) } },

	{ "Targo",		"Tank",		"Sledgehammer",		"stormtrooper",		150, 940, 48, 100, 30, 2,
	{ MOBA_AB( "Sledgehammer",		AB_DIRECT,		6500,	25,	100,	20,	850,	0,	0,	1.0f, "Sledgehammer" ),
	  MOBA_AB( "Siege",				AB_AOE_DAMAGE,	13000,	50,	75,	16,	0,		500,	0,	1.0f, "Siege" ) } },

	{ "Velia",		"Mage",		"Saber + Force",		"reborn",	320, 530, 26, 50, 20, 2,
	{ MOBA_AB( "Stardust",			AB_DIRECT,		5500,	30,	115,	26,	1000,	0,	0,	1.0f, "Dust" ),
	  MOBA_AB( "Meteor",			AB_AOE_DAMAGE,	10500,	50,	82,	18,	0,		430,	0,	1.0f, "Meteor" ) } },

	{ "Astarot",		"Assassin",	"Saber + Force",	"rosh_penin",	180, 530, 25, 45, 44, 4,
	{ MOBA_AB( "Demonic Claw",		AB_DIRECT,		4500,	30,	135,	34,	1000,	0,	0,	1.0f, "Claw" ),
	  MOBA_AB( "Inferno Flame",		AB_AOE_DAMAGE,	9500,	45,	85,	18,	0,		380,	0,	1.0f, "Flame" ) } },

	{ "Belin",		"Healer",	"Saber + Force",	"cultist",	260, 620, 31, 60, 22, 1,
	{ MOBA_AB( "Healing Ring",		AB_AOE_HEAL,	5500,	30,	70,	13,	0,		550,	0,	1.0f, "Ring" ),
	  MOBA_AB( "Ray of Light",		AB_DIRECT,		7500,	25,	75,	15,	1000,	0,	0,	1.0f, "Beam" ) } },

	{ "Draks",		"Tank",		"Saber + Force",		"human_merc",	150, 960, 52, 100, 28, 2,
	{ MOBA_AB( "Fire Strike",		AB_DIRECT,		6000,	25,	95,	20,	900,	0,	0,	1.0f, "Strike" ),
	  MOBA_AB( "Flame Circle",		AB_AOE_DAMAGE,	12000,	45,	72,	15,	0,		460,	0,	1.0f, "Circle" ) } },

	{ "Elfin",		"Carry",	"Bowcaster",	"stormpilot",		200, 580, 30, 60, 40, 3,
	{ MOBA_AB( "Rapid Shot",		AB_DIRECT,		5000,	25,	95,	22,	1150,	0,	0,	1.0f, "Shot" ),
	  MOBA_AB( "Arrow Lightning",	AB_DIRECT,		9000,	45,	140,	30,	1100,	0,	0,	1.0f, "Lightning" ) } },

	{ "Nomara",		"Mage",		"Saber + Force",		"reborn_new",	300, 550, 28, 55, 22, 2,
	{ MOBA_AB( "Water Wave",		AB_DIRECT,		6000,	30,	100,	22,	1000,	0,	0,	1.0f, "Wave" ),
	  MOBA_AB( "Deluge",			AB_AOE_DAMAGE,	10000,	50,	72,	16,	0,		440,	0,	1.0f, "Deluge" ) } },

	{ "Quinn",		"Assassin",	"Twin sabers",	"morgan",		180, 510, 23, 40, 45, 4,
	{ MOBA_AB( "Claw Shadow",		AB_DIRECT,		4500,	35,	140,	36,	1000,	0,	0,	1.0f, "Shadow claw" ),
	  MOBA_AB( "Shadow Storm",		AB_AOE_DAMAGE,	9500,	45,	90,	20,	0,		400,	0,	1.0f, "Storm" ) } },

	{ "Charon",		"Tank",		"Saber + Force",		"saboteur",	150, 970, 54, 100, 26, 2,
	{ MOBA_AB( "Bone Strike",		AB_DIRECT,		6500,	20,	88,	17,	850,	0,	0,	1.0f, "Strike" ),
	  MOBA_AB( "Bone Wall",			AB_AOE_DAMAGE,	13000,	50,	70,	14,	0,		470,	0,	1.0f, "Wall" ) } },

	{ "Ignis",		"Mage",		"Saber + Force",		"reborn_twin",	320, 520, 25, 50, 22, 2,
	{ MOBA_AB( "Spark",				AB_DIRECT,		5000,	35,	120,	28,	1000,	0,	0,	1.0f, "Spark" ),
	  MOBA_AB( "Arson",				AB_AOE_DAMAGE,	9500,	50,	85,	18,	0,		420,	0,	1.0f, "Arson" ) } },

	{ "Valka",		"Healer",	"Saber + Force",	"prisoner",	280, 630, 33, 65, 22, 1,
	{ MOBA_AB( "Healing Dew",		AB_AOE_HEAL,	5000,	30,	70,	14,	0,		600,	0,	1.0f, "Dew" ),
	  MOBA_AB( "Ray of Dawn",		AB_DIRECT,		8000,	30,	85,	17,	1000,	0,	0,	1.0f, "Beam" ) } },

	{ "sr.Ronald",	"Tank",		"Single sword",		"dindjarin",	150, 980, 55, 100, 30, 2,
	{ MOBA_AB( "Heroic Leap",		AB_LEAP,		15000,	30,	50,	10,	750,	400,	0,	1.0f, "Leap forward, knock down and 50 damage on landing" ),
	  MOBA_AB( "Bulwark",			AB_SHIELD,		20000,	30,	150,	50,	0,		0,		5000,	1.0f, "Shield absorbs damage for 5 s" ) } },

	{ "Peroohn",	"Mage",		"Dual sabers",		"shadowspawn",	300, 560, 28, 50, 22, 2,
	{ MOBA_AB( "Fireball",			AB_PROJECTILE,	15000,	15,	20,	5,	1100,	0,		4000,	0.10f, "Bolt: 20 damage + 10% slow for 4 s, 4 charges" ),
	  MOBA_AB( "Flame Stream",		AB_FLAME,		0,		20,	15,	0,	700,	0,		0,		1.0f, "Held flame: 15 damage and 20 mana per second" ) } },

	{ "Bishop Panteleimon",	"Mage",	"Dual sabers",	"cultist",	300, 540, 26, 50, 22, 2,
	{ MOBA_AB( "Holy Silence",		AB_SILENCE,		45000,	70,	0,	0,	0,		1000,	10000,	1.0f, "Silence all enemies within 1000 for 10 s" ),
	  MOBA_AB( "Aegis of Faith",	AB_MAGICRESIST,	45000,	80,	0,	0,	0,		500,	15000,	0.5f, "Allies within 500 take half magic damage for 15 s" ) } },
};

#endif // MOBA_CONTENT_H
