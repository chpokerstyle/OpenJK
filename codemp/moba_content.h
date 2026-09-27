/*
===========================================================================
MagicWands MOBA mod - content shared by the game and the cgame
===========================================================================
The hero table is content, not state: the server owns the authoritative copy
in mobaHeroes[] while the cgame has to render the very same numbers, names and
ability values in the hero select window. Two hand written copies of thirty
heroes would drift apart within a day, so the table itself lives here and both
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

#define MOBA_MAX_HEROES			30
#define MOBA_ABILITIES_PER_HERO	4

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
	const char		*weapons;		// loadout flavour, shown in the info panel
	const char		*model;		// models/players/<model>, the body the player spawns as
	int				baseHealth;
	int				healthPerLevel;
	int				baseArmor;
	int				baseDamage;		// reserved for auto-attack scaling
	int				damagePerLevel;
	mobaAbility_t	abilities[MOBA_ABILITIES_PER_HERO];
} mobaHero_t;

#define MOBA_AB(n,t,cd,d,pl,r,rad,dur,mul,desc) { n, t, cd, d, pl, r, rad, dur, mul, desc }

static const mobaHero_t mobaHeroTable[MOBA_MAX_HEROES] = {
	{ "Ash'Lar",		"Tank",		"Lightsaber",		"kyle",		950, 50, 100, 30, 2,
	{ MOBA_AB( "Piercing Blade",	AB_DIRECT,		6000,	90,	20,	900,	0,	0,	1.0f, "Hit target" ),
	  MOBA_AB( "Blade Wall",		AB_AOE_DAMAGE,	12000,	70,	15,	0,		450,	0,	1.0f, "Sweeping strike" ),
	  MOBA_AB( "Battle Rage",		AB_BUFF,		20000,	0,	0,	0,		0,		9000,	1.6f, "+60% dmg 9s" ),
	  MOBA_AB( "Earth Rift",		AB_AOE_DAMAGE,	30000,	150,	30,	0,		600,	0,	1.0f, "Powerful shock" ) } },

	{ "Vectar",		"Tank",		"War hammer",		"lando",		920, 48, 100, 28, 2,
	{ MOBA_AB( "Thunder Hammer",		AB_DIRECT,		7000,	95,	18,	800,	0,	0,	1.0f, "Stunning blow" ),
	  MOBA_AB( "Stone Skin",		AB_BUFF,		18000,	0,	0,	0,		0,		10000,	1.35f, "+35% dmg 10s" ),
	  MOBA_AB( "Shockwave",			AB_AOE_DAMAGE,	11000,	60,	12,	0,		500,	0,	1.0f, "Area blast" ),
	  MOBA_AB( "Golem Wrath",		AB_AOE_DAMAGE,	28000,	130,	25,	0,		550,	0,	1.0f, "Shatter" ) } },

	{ "T'Raine",		"Tank",		"Short sword",		"imperial",		980, 55, 100, 26, 2,
	{ MOBA_AB( "Spike",				AB_DIRECT,		6500,	85,	16,	850,	0,	0,	1.0f, "Piercing thrust" ),
	  MOBA_AB( "Iron Ring",			AB_AOE_DAMAGE,	12000,	65,	14,	0,		450,	0,	1.0f, "Ring of damage" ),
	  MOBA_AB( "Unyielding",			AB_BUFF,		22000,	0,	0,	0,		0,		8000,	1.5f, "+50% dmg 8s" ),
	  MOBA_AB( "Meat Grinder",		AB_AOE_DAMAGE,	32000,	160,	35,	0,		500,	0,	1.0f, "Whirling storm" ) } },

	{ "Korkhin",		"Mage",		"Saber + Force",		"jedi",	560, 28, 50, 22, 2,
	{ MOBA_AB( "Lightning Lash",		AB_DIRECT,		6000,	110,	25,	1000,	0,	0,	1.0f, "Lightning at target" ),
	  MOBA_AB( "Fireball",			AB_AOE_DAMAGE,	10000,	85,	18,	0,		400,	0,	1.0f, "Explosion around self" ),
	  MOBA_AB( "Arc Chain",			AB_DIRECT,		9000,	125,	22,	950,	0,	0,	1.0f, "Strong discharge" ),
	  MOBA_AB( "Storm",				AB_AOE_DAMAGE,	35000,	200,	45,	0,		600,	0,	1.0f, "Area storm" ) } },

	{ "Silvara",		"Mage",		"Saber + Force",		"chiss",	540, 26, 50, 20, 2,
	{ MOBA_AB( "Ice Dagger",		AB_DIRECT,		5500,	105,	24,	1000,	0,	0,	1.0f, "Ice at target" ),
	  MOBA_AB( "Frost Breath",		AB_BUFF,		16000,	0,	0,	0,		0,		7000,	1.4f, "+40% dmg 7s" ),
	  MOBA_AB( "Hail",				AB_AOE_DAMAGE,	11000,	75,	16,	0,		450,	0,	1.0f, "Hail around" ),
	  MOBA_AB( "Eternal Winter",	AB_AOE_DAMAGE,	34000,	190,	40,	0,		550,	0,	1.0f, "Freezing storm" ) } },

	{ "Merek",		"Mage",		"Saber + Force",		"jeditrainer",	580, 30, 55, 24, 2,
	{ MOBA_AB( "Spirit Fire",		AB_DIRECT,		6500,	115,	20,	950,	0,	0,	1.0f, "Flaming beam" ),
	  MOBA_AB( "Fire Cocktail",		AB_AOE_DAMAGE,	10000,	80,	20,	0,		420,	0,	1.0f, "Explosion" ),
	  MOBA_AB( "Flame Shield",		AB_BUFF,		20000,	0,	0,	0,		0,		9000,	1.55f, "+55% dmg 9s" ),
	  MOBA_AB( "Ash Rain",			AB_AOE_DAMAGE,	33000,	195,	42,	0,		580,	0,	1.0f, "Firestorm" ) } },

	{ "Ornat",		"Mage",		"Saber + Force",		"tavion",	520, 25, 50, 22, 2,
	{ MOBA_AB( "Acid Shot",		AB_DIRECT,		6000,	120,	28,	1000,	0,	0,	1.0f, "Acid" ),
	  MOBA_AB( "Rot Wave",			AB_AOE_DAMAGE,	11000,	70,	15,	0,		430,	0,	1.0f, "Rot around" ),
	  MOBA_AB( "Evil Eye",			AB_BUFF,		17000,	0,	0,	0,		0,		8000,	1.45f, "+45% dmg 8s" ),
	  MOBA_AB( "Plague Column",		AB_AOE_DAMAGE,	36000,	210,	50,	0,		600,	0,	1.0f, "Giant plague" ) } },

	{ "Zum'Zar",		"Mage",		"Saber + Force",		"tavion_new",	550, 27, 55, 22, 2,
	{ MOBA_AB( "Thunder Strike",	AB_DIRECT,		7000,	130,	26,	900,	0,	0,	1.0f, "Thunder" ),
	  MOBA_AB( "Thunderclap",		AB_AOE_DAMAGE,	11000,	78,	17,	0,		480,	0,	1.0f, "Shock wave" ),
	  MOBA_AB( "Energy Charge",		AB_BUFF,		19000,	0,	0,	0,		0,		8000,	1.5f, "+50% dmg 8s" ),
	  MOBA_AB( "Thunder Burst",	AB_AOE_DAMAGE,	32000,	180,	38,	0,		560,	0,	1.0f, "Sky rupture" ) } },

	{ "Killian",		"Carry",	"Blaster rifle",	"rockettrooper",	620, 32, 60, 35, 3,
	{ MOBA_AB( "Precision Shot",		AB_DIRECT,		5000,	100,	22,	1100,	0,	0,	1.0f, "Shot" ),
	  MOBA_AB( "Rapid Fire",		AB_BUFF,		14000,	0,	0,	0,		0,		6000,	1.5f, "+50% dmg 6s" ),
	  MOBA_AB( "Shrapnel",			AB_AOE_DAMAGE,	10000,	65,	14,	0,		380,	0,	1.0f, "Shards" ),
	  MOBA_AB( "Golden Bullet",		AB_DIRECT,		26000,	230,	50,	1200,	0,	0,	1.0f, "Lethal shot" ) } },

	{ "Dinara",		"Carry",	"Twin sabers",	"shadowtrooper",		600, 30, 65, 38, 3,
	{ MOBA_AB( "Twin Blades",		AB_DIRECT,		5500,	95,	20,	1000,	0,	0,	1.0f, "Double strike" ),
	  MOBA_AB( "Blade Dance",		AB_AOE_DAMAGE,	12000,	70,	15,	0,		420,	0,	1.0f, "Ring of blades" ),
	  MOBA_AB( "Blades of Greed",	AB_BUFF,		18000,	0,	0,	0,		0,		9000,	1.45f, "+45% dmg 9s" ),
	  MOBA_AB( "Hidden Slash",		AB_DIRECT,		28000,	210,	45,	1000,	0,	0,	1.0f, "Cutting sweep" ) } },

	{ "Starr",		"Carry",	"Heavy blaster",	"snowtrooper",	640, 34, 70, 36, 3,
	{ MOBA_AB( "Assault Volley",		AB_DIRECT,		5000,	85,	18,	1050,	0,	0,	1.0f, "Volley" ),
	  MOBA_AB( "Roaring Barrage",	AB_AOE_DAMAGE,	11000,	60,	15,	0,		400,	0,	1.0f, "Wave" ),
	  MOBA_AB( "Adrenaline",		AB_BUFF,		15000,	0,	0,	0,		0,		7000,	1.55f, "+55% dmg 7s" ),
	  MOBA_AB( "Burst Rounds",		AB_DIRECT,		27000,	220,	48,	1150,	0,	0,	1.0f, "Burst" ) } },

	{ "Brock",		"Carry",	"War axe",	"hazardtrooper",			660, 35, 70, 34, 3,
	{ MOBA_AB( "Chopping Blow",		AB_DIRECT,		6000,	90,	19,	950,	0,	0,	1.0f, "Axe" ),
	  MOBA_AB( "Whirl",				AB_AOE_DAMAGE,	11000,	65,	14,	0,		420,	0,	1.0f, "Axe whirlwind" ),
	  MOBA_AB( "Beast Rage",		AB_BUFF,		17000,	0,	0,	0,		0,		8000,	1.5f, "+50% dmg 8s" ),
	  MOBA_AB( "Crusher",			AB_DIRECT,		30000,	240,	55,	1000,	0,	0,	1.0f, "All-out strike" ) } },

	{ "Lira",		"Healer",	"Lightsaber",	"monmothma",		600, 30, 60, 22, 1,
	{ MOBA_AB( "Light Discipline",	AB_AOE_HEAL,	6000,	80,	15,	0,		600,	0,	1.0f, "Area heal" ),
	  MOBA_AB( "Ray of Hope",		AB_DIRECT,		8000,	70,	14,	950,	0,	0,	1.0f, "Beam" ),
	  MOBA_AB( "Blessing",			AB_BUFF,		18000,	0,	0,	0,		0,		9000,	1.4f, "+40% dmg 9s" ),
	  MOBA_AB( "Greater Heal",		AB_AOE_HEAL,	26000,	220,	40,	0,		700,	0,	1.0f, "Powerful heal" ) } },

	{ "Selena",		"Healer",	"Light spear",	"jan",		580, 28, 55, 20, 1,
	{ MOBA_AB( "Wave of Life",		AB_AOE_HEAL,	6500,	75,	14,	0,		550,	0,	1.0f, "Heal" ),
	  MOBA_AB( "Light Spear",		AB_DIRECT,		7000,	80,	16,	1000,	0,	0,	1.0f, "Spear" ),
	  MOBA_AB( "Inspiration",		AB_BUFF,		20000,	0,	0,	0,		0,		10000,	1.35f, "+35% dmg 10s" ),
	  MOBA_AB( "Wound Refresh",		AB_AOE_HEAL,	25000,	200,	38,	0,		650,	0,	1.0f, "Full heal" ) } },

	{ "Mornan",		"Healer",	"War hammer",	"gran",		640, 32, 65, 24, 1,
	{ MOBA_AB( "Balm",				AB_AOE_HEAL,	6000,	85,	16,	0,		580,	0,	1.0f, "Heal" ),
	  MOBA_AB( "Hammer of Fate",		AB_DIRECT,		8500,	90,	18,	900,	0,	0,	1.0f, "Hammer" ),
	  MOBA_AB( "Fortitude",			AB_BUFF,		19000,	0,	0,	0,		0,		9000,	1.3f, "+30% dmg 9s" ),
	  MOBA_AB( "Healer's Hands",		AB_AOE_HEAL,	24000,	240,	45,	0,		700,	0,	1.0f, "Full heal" ) } },

	{ "Gillian",		"Assassin",	"Twin sabers",	"alora",		540, 26, 45, 40, 4,
	{ MOBA_AB( "Shadow Stab",		AB_DIRECT,		4500,	130,	30,	1000,	0,	0,	1.0f, "Stab" ),
	  MOBA_AB( "Shadow Blades",		AB_AOE_DAMAGE,	10000,	80,	18,	0,		400,	0,	1.0f, "Blade circles" ),
	  MOBA_AB( "Shadow Rage",		AB_BUFF,		14000,	0,	0,	0,		0,		6000,	1.6f, "+60% dmg 6s" ),
	  MOBA_AB( "Deadly Slash",		AB_DIRECT,		24000,	250,	60,	1100,	0,	0,	1.0f, "Lethal strike" ) } },

	{ "Kyra",		"Assassin",	"Saber + Force",	"alora2",	520, 24, 45, 42, 4,
	{ MOBA_AB( "Backstab",			AB_DIRECT,		5000,	120,	28,	950,	0,	0,	1.0f, "Dagger" ),
	  MOBA_AB( "Blood Dance",		AB_AOE_DAMAGE,	10000,	75,	16,	0,		380,	0,	1.0f, "Dance" ),
	  MOBA_AB( "Hunter's Zeal",		AB_BUFF,		13000,	0,	0,	0,		0,		7000,	1.55f, "+55% dmg 7s" ),
	  MOBA_AB( "Piercing Shadow",	AB_DIRECT,		25000,	260,	55,	1150,	0,	0,	1.0f, "Shadow slash" ) } },

	{ "Ravel",		"Assassin",	"Twin sabers",	"reelo",		560, 27, 50, 38, 4,
	{ MOBA_AB( "Knife Whirl",		AB_DIRECT,		4800,	110,	26,	1050,	0,	0,	1.0f, "Whirl" ),
	  MOBA_AB( "Wind Blades",		AB_AOE_DAMAGE,	10000,	70,	15,	0,		420,	0,	1.0f, "Blades" ),
	  MOBA_AB( "Aggression",		AB_BUFF,		15000,	0,	0,	0,		0,		8000,	1.5f, "+50% dmg 8s" ),
	  MOBA_AB( "Deadly Storm",		AB_DIRECT,		23000,	230,	55,	1200,	0,	0,	1.0f, "Lethal storm" ) } },

	{ "Ismara",		"Carry",	"Blaster rifle",	"rebel_pilot",	600, 31, 65, 37, 3,
	{ MOBA_AB( "Fire Volley",		AB_DIRECT,		5500,	105,	24,	1050,	0,	0,	1.0f, "Volley" ),
	  MOBA_AB( "Burst Fire",		AB_AOE_DAMAGE,	10000,	70,	15,	0,		400,	0,	1.0f, "Barrage" ),
	  MOBA_AB( "Warrior's Aim",	AB_BUFF,		16000,	0,	0,	0,		0,		7000,	1.45f, "+45% dmg 7s" ),
	  MOBA_AB( "Finishing Shot",	AB_DIRECT,		25000,	225,	50,	1200,	0,	0,	1.0f, "Harpoon shot" ) } },

	{ "Targo",		"Tank",		"Sledgehammer",		"stormtrooper",		940, 48, 100, 30, 2,
	{ MOBA_AB( "Sledgehammer",		AB_DIRECT,		6500,	100,	20,	850,	0,	0,	1.0f, "Sledgehammer" ),
	  MOBA_AB( "Siege",				AB_AOE_DAMAGE,	13000,	75,	16,	0,		500,	0,	1.0f, "Siege" ),
	  MOBA_AB( "Armored Assault",	AB_BUFF,		21000,	0,	0,	0,		0,		10000,	1.4f, "+40% dmg 10s" ),
	  MOBA_AB( "Demolition",		AB_AOE_DAMAGE,	30000,	145,	28,	0,		560,	0,	1.0f, "Demolition" ) } },

	{ "Velia",		"Mage",		"Saber + Force",		"reborn",	530, 26, 50, 20, 2,
	{ MOBA_AB( "Stardust",			AB_DIRECT,		5500,	115,	26,	1000,	0,	0,	1.0f, "Dust" ),
	  MOBA_AB( "Meteor",			AB_AOE_DAMAGE,	10500,	82,	18,	0,		430,	0,	1.0f, "Meteor" ),
	  MOBA_AB( "Star Rage",			AB_BUFF,		18000,	0,	0,	0,		0,		9000,	1.5f, "+50% dmg 9s" ),
	  MOBA_AB( "World Fall",		AB_AOE_DAMAGE,	34000,	205,	44,	0,		600,	0,	1.0f, "New worlds" ) } },

	{ "Astarot",		"Assassin",	"Saber + Force",	"rosh_penin",	530, 25, 45, 44, 4,
	{ MOBA_AB( "Demonic Claw",		AB_DIRECT,		4500,	135,	34,	1000,	0,	0,	1.0f, "Claw" ),
	  MOBA_AB( "Inferno Flame",		AB_AOE_DAMAGE,	9500,	85,	18,	0,		380,	0,	1.0f, "Flame" ),
	  MOBA_AB( "Bloodthirst",		AB_BUFF,		12000,	0,	0,	0,		0,		6000,	1.65f, "+65% dmg 6s" ),
	  MOBA_AB( "Death Ritual",		AB_DIRECT,		22000,	270,	65,	1100,	0,	0,	1.0f, "Lethal ritual" ) } },

	{ "Belin",		"Healer",	"Saber + Force",	"cultist",	620, 31, 60, 22, 1,
	{ MOBA_AB( "Healing Ring",		AB_AOE_HEAL,	5500,	70,	13,	0,		550,	0,	1.0f, "Ring" ),
	  MOBA_AB( "Ray of Light",		AB_DIRECT,		7500,	75,	15,	1000,	0,	0,	1.0f, "Beam" ),
	  MOBA_AB( "Prayer",			AB_BUFF,		20000,	0,	0,	0,		0,		10000,	1.3f, "+30% dmg 10s" ),
	  MOBA_AB( "Great Miracle",		AB_AOE_HEAL,	24000,	210,	40,	0,		700,	0,	1.0f, "Miracle" ) } },

	{ "Draks",		"Tank",		"Saber + Force",		"human_merc",	960, 52, 100, 28, 2,
	{ MOBA_AB( "Fire Strike",		AB_DIRECT,		6000,	95,	20,	900,	0,	0,	1.0f, "Strike" ),
	  MOBA_AB( "Flame Circle",		AB_AOE_DAMAGE,	12000,	72,	15,	0,		460,	0,	1.0f, "Circle" ),
	  MOBA_AB( "Dragon Rage",		AB_BUFF,		19000,	0,	0,	0,		0,		9000,	1.55f, "+55% dmg 9s" ),
	  MOBA_AB( "Dragon Breath",		AB_AOE_DAMAGE,	31000,	160,	32,	0,		580,	0,	1.0f, "Flaming breath" ) } },

	{ "Elfin",		"Carry",	"Bowcaster",	"stormpilot",		580, 30, 60, 40, 3,
	{ MOBA_AB( "Rapid Shot",		AB_DIRECT,		5000,	95,	22,	1150,	0,	0,	1.0f, "Shot" ),
	  MOBA_AB( "Arrow Lightning",	AB_DIRECT,		9000,	140,	30,	1100,	0,	0,	1.0f, "Lightning" ),
	  MOBA_AB( "Swiftness",		AB_BUFF,		15000,	0,	0,	0,		0,		7000,	1.5f, "+50% dmg 7s" ),
	  MOBA_AB( "Dragon Shot",		AB_DIRECT,		24000,	215,	48,	1300,	0,	0,	1.0f, "Dragon arrow" ) } },

	{ "Nomara",		"Mage",		"Saber + Force",		"reborn_new",	550, 28, 55, 22, 2,
	{ MOBA_AB( "Water Wave",		AB_DIRECT,		6000,	100,	22,	1000,	0,	0,	1.0f, "Wave" ),
	  MOBA_AB( "Deluge",			AB_AOE_DAMAGE,	10000,	72,	16,	0,		440,	0,	1.0f, "Deluge" ),
	  MOBA_AB( "Power Surge",		AB_BUFF,		17000,	0,	0,	0,		0,		8000,	1.45f, "+45% dmg 8s" ),
	  MOBA_AB( "Ocean's Wrath",		AB_AOE_DAMAGE,	33000,	185,	40,	0,		570,	0,	1.0f, "Ocean" ) } },

	{ "Quinn",		"Assassin",	"Twin sabers",	"morgan",		510, 23, 40, 45, 4,
	{ MOBA_AB( "Claw Shadow",		AB_DIRECT,		4500,	140,	36,	1000,	0,	0,	1.0f, "Shadow claw" ),
	  MOBA_AB( "Shadow Storm",		AB_AOE_DAMAGE,	9500,	90,	20,	0,		400,	0,	1.0f, "Storm" ),
	  MOBA_AB( "Dark Grasp",		AB_BUFF,		11000,	0,	0,	0,		0,		6000,	1.7f, "+70% dmg 6s" ),
	  MOBA_AB( "Rending Claw",		AB_DIRECT,		21000,	280,	70,	1100,	0,	0,	1.0f, "Burst" ) } },

	{ "Charon",		"Tank",		"Saber + Force",		"saboteur",	970, 54, 100, 26, 2,
	{ MOBA_AB( "Bone Strike",		AB_DIRECT,		6500,	88,	17,	850,	0,	0,	1.0f, "Strike" ),
	  MOBA_AB( "Bone Wall",			AB_AOE_DAMAGE,	13000,	70,	14,	0,		470,	0,	1.0f, "Wall" ),
	  MOBA_AB( "Cold Wrath",		AB_BUFF,		20000,	0,	0,	0,		0,		9000,	1.5f, "+50% dmg 8s" ),
	  MOBA_AB( "Death and Bones",	AB_AOE_DAMAGE,	29000,	150,	30,	0,		540,	0,	1.0f, "Bone storm" ) } },

	{ "Ignis",		"Mage",		"Saber + Force",		"reborn_twin",	520, 25, 50, 22, 2,
	{ MOBA_AB( "Spark",				AB_DIRECT,		5000,	120,	28,	1000,	0,	0,	1.0f, "Spark" ),
	  MOBA_AB( "Arson",				AB_AOE_DAMAGE,	9500,	85,	18,	0,		420,	0,	1.0f, "Arson" ),
	  MOBA_AB( "Burning Blood",		AB_BUFF,		15000,	0,	0,	0,		0,		7000,	1.5f, "+50% dmg 8s" ),
	  MOBA_AB( "Burning World",		AB_AOE_DAMAGE,	32000,	200,	42,	0,		600,	0,	1.0f, "World of fire" ) } },

	{ "Valka",		"Healer",	"Saber + Force",	"prisoner",	630, 33, 65, 22, 1,
	{ MOBA_AB( "Healing Dew",		AB_AOE_HEAL,	5000,	70,	14,	0,		600,	0,	1.0f, "Dew" ),
	  MOBA_AB( "Ray of Dawn",		AB_DIRECT,		8000,	85,	17,	1000,	0,	0,	1.0f, "Beam" ),
	  MOBA_AB( "Dawn Charge",		AB_BUFF,		18000,	0,	0,	0,		0,		10000,	1.4f, "+40% dmg 10s" ),
	  MOBA_AB( "Daybreak",			AB_AOE_HEAL,	23000,	230,	42,	0,		700,	0,	1.0f, "Greater heal" ) } }
};

#endif // MOBA_CONTENT_H
