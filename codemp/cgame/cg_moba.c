/*
===========================================================================
MagicWands MOBA mod - OpenJK cgame (hero window, shop window, ability bar)
===========================================================================
The server pushes "mobaShop phase seconds gold itemMask level" to the client
once per change. That is the only thing the shop needs to know, so the window
stays in sync with the authoritative game state without any guessing.
===========================================================================
*/

#include "cg_local.h"
#include "ui/ui_shared.h"	// for Menu_FindItemByName (and menuDef_t) first: the header
							// declares CG_Moba_DrawMana( struct menuDef_s * ) below
#include "cg_moba.h"
#include "moba_content.h"
#include "ui/keycodes.h"
#include "ui/menudef.h"

//=========================================================================
// Presentation copy of mobaItems from g_moba.c. The server stays the only
// owner of the prices it actually charges, this table only has to render them.
// The category is the index of the tab in cgMobaItemCats, one item belongs to
// exactly one tab and the tiles inside a tab follow the order of this table.
//=========================================================================
typedef struct {
	const char	*name;
	int			price;
	const char	*desc;
	int			category;		// index into cgMobaItemCats
	int			maxCount;		// how many copies fit in one slot
	int			cooldownMs;		// how long the slot is locked after a use
	int			manaCost;
	int			durationMs;
} cgMobaItem_t;

static const char *cgMobaItemCats[] = { "DEFENCE", "ATTACK", "CONSUMABLES" };
#define CG_MOBA_ITEM_CATS	( (int)( sizeof( cgMobaItemCats ) / sizeof( cgMobaItemCats[0] ) ) )

// The order is the order of mobaItems on the server, because the index of a row
// is the bit the item mask uses and the id the server pushes for a slot, so the
// two tables may never be sorted apart. The prices and the numbers below come
// from moba_content.h, the same header the server prices its items from, so a
// rebalance cannot reach one side only.
static const cgMobaItem_t cgMobaItems[] = {
	{ "Sturdy Armor",		250,	"+50 armor",						0,	1,	0,
		0,	0 },
	{ "Med Kit",			200,	"heal 100 to a nearby ally",		2,	1,	MOBA_ITEM_MEDKIT_CD,
		0,	MOBA_ITEM_MEDKIT_HEAL },
	{ "Rage Rune",			300,	"+20% damage",					1,	1,	0,
		0,	0 },
	{ "Heavy Plate",		500,	"+100 armor, +50 health",			0,	1,	0,
		0,	0 },
	{ "Power Crystal",		650,	"+50 health, +40% damage",		1,	1,	0,
		0,	0 },
	{ "Shadow Cloak",		400,	"+30 armor, +15% damage",			0,	1,	0,
		0,	0 },
	{ "Grenade",			75,		"throw a grenade, 4 per player",	2,	MOBA_ITEM_GRENADE_MAX,	MOBA_ITEM_GRENADE_CD,
		0,	0 },
	{ "Umbrella",			1000,	"+50 armor for 20 s",				0,	1,	MOBA_ITEM_UMBRELLA_DUR + MOBA_ITEM_UMBRELLA_CD,
		MOBA_ITEM_UMBRELLA_MANA,	MOBA_ITEM_UMBRELLA_DUR },
	{ "Invisibility Cloak",	600,	"hidden from enemies, a ghost to allies",	0,	1,	MOBA_ITEM_CLOAK_CD,
		MOBA_ITEM_CLOAK_MANA,	MOBA_ITEM_CLOAK_DUR }
};

#define CG_MOBA_NUM_ITEMS	( (int)( sizeof( cgMobaItems ) / sizeof( cgMobaItems[0] ) ) )

// mirrors mobaPhase_t from g_moba.h, keep both in sync: LOBBY 0, DRAFT 1,
// DRAFT_ASSIGN 2, BUY 3, FIGHT 4, ROUNDEND 5
#define CG_MOBA_PHASE_DRAFT			1
#define CG_MOBA_PHASE_DRAFT_ASSIGN	2
#define CG_MOBA_PHASE_BUY			3
#define CG_MOBA_PHASE_FIGHT			4

//=========================================================================
// State received from the server plus the local "the player closed it again"
// flag. The countdown arrives in whole seconds, so it is continued locally
// from the moment the update arrived.
//=========================================================================
typedef struct {
	int			phase;
	int			secondsLeft;
	int			receivedAt;
	int			gold;
	int			itemMask;
	int			level;
	int			noticeUntil;		// "only in the buy phase" message
	int			category;		// tab of the shop window
	int			cursor;			// grid slot under the keyboard cursor, -1 = none
	int			slotItem[2];
	int			slotCount[2];
	int			slotCd[2];
	qboolean	open;				// the player opened the shop with B
	qboolean	hadCatcher;		// the window currently owns the mouse
	qboolean	received;
	qboolean	logged;			// one time confirmation in the client log
} cgMobaState_t;

static cgMobaState_t cgMoba;

//=========================================================================
// Ability bar state. The server owns the cooldowns and the bought ranks and
// pushes them five times a second, the client counts the remaining time down in
// between so the bar does not stutter. heroId is -1 while the player has no
// hero, and the bar is then not drawn at all.
//=========================================================================
#define CG_MOBA_ABILITIES	2

typedef struct {
	int			heroId;
	int			cooldown[CG_MOBA_ABILITIES];	// ms left at the moment of the push
	int			level[CG_MOBA_ABILITIES];
	int			effect[CG_MOBA_ABILITIES];	// ms left of a buff this slot put on the caster
	int			mana;			// current mana, snapshot of the last push
	int			maxMana;		// mana pool of the hero
	int			receivedAt;
	qboolean	received;
	qboolean	loggedHero;		// -2 before the first push, so the first one logs
	qboolean	loggedCooldown;	// a running timer has been seen at least once
} cgMobaAbility_t;

static cgMobaAbility_t cgMobaAb;

// what is left of a cooldown right now, the push only samples it
static int CG_Moba_CooldownLeft( int slot )
{
	int left;

	if ( !cgMobaAb.received || slot < 0 || slot >= CG_MOBA_ABILITIES )
	{
		return 0;
	}

	left = cgMobaAb.cooldown[slot] - ( cg.time - cgMobaAb.receivedAt );

	return ( left > 0 ) ? left : 0;
}

// what is left of a running effect of that slot, the push only samples it too
static int CG_Moba_EffectLeft( int slot )
{
	int left;

	if ( !cgMobaAb.received || slot < 0 || slot >= CG_MOBA_ABILITIES )
	{
		return 0;
	}

	left = cgMobaAb.effect[slot] - ( cg.time - cgMobaAb.receivedAt );

	return ( left > 0 ) ? left : 0;
}

//=========================================================================
// Hero select window
//
// The window is a modal panel: it takes the mouse from the game (see
// CG_Moba_Catcher) so the player can look at the board instead of at the
// world, and it draws every hero of mobaHeroTable with the ban and pick state
// the server pushed. Nothing here decides anything, a click only turns into
// the same !pick / !ban command a player would type in chat.
//=========================================================================
#define CG_MOBA_WIN_X			20.0f
#define CG_MOBA_WIN_Y			28.0f
#define CG_MOBA_WIN_W			600.0f
#define CG_MOBA_WIN_H			428.0f

#define CG_MOBA_COLS			5
#define CG_MOBA_ROWS			6
#define CG_MOBA_TILE_W			76.0f
#define CG_MOBA_TILE_H			52.0f
#define CG_MOBA_TILE_GAP		3.0f

#define CG_MOBA_GRID_X			( CG_MOBA_WIN_X + 8.0f )
#define CG_MOBA_GRID_Y			( CG_MOBA_WIN_Y + 48.0f )
#define CG_MOBA_INFO_X			( CG_MOBA_GRID_X + CG_MOBA_COLS * ( CG_MOBA_TILE_W + CG_MOBA_TILE_GAP ) + 8.0f )
#define CG_MOBA_INFO_W			( CG_MOBA_WIN_X + CG_MOBA_WIN_W - 8.0f - CG_MOBA_INFO_X )

// the confirm button sits under the board, a left click on a tile only arms it
#define CG_MOBA_BTN_X			( CG_MOBA_WIN_X + 8.0f )
#define CG_MOBA_BTN_Y			( CG_MOBA_WIN_Y + CG_MOBA_WIN_H - 32.0f )
#define CG_MOBA_BTN_W			214.0f
#define CG_MOBA_BTN_H			24.0f

#define CG_MOBA_ACT_NONE		0
#define CG_MOBA_ACT_BAN			1
#define CG_MOBA_ACT_PICK		2

// hero selection mode of the server, mirrors moba_mode of g_moba.c
#define CG_MOBA_MODE_CAPTAIN	0		// captains ban and pick a team pool
#define CG_MOBA_MODE_ALLPICK	1		// no bans, everybody takes a free hero

typedef struct {
	unsigned long long bannedMask;	// one bit per hero, index into mobaHeroTable
	unsigned long long redMask;
	unsigned long long blueMask;
	unsigned long long takenMask;	// heroes a player has already taken
	int			action;			// CG_MOBA_ACT_*, what the current step is
	int			canAct;			// only the server may answer this
	int			secondsLeft;
	int			receivedAt;
	int			myHero;			// -1 while the player has no hero
	int			myTeam;			// 0 none, 1 red, 2 blue (team_t on the server)
	int			step, steps;
	int			mode;			// CG_MOBA_MODE_*, hero selection mode of the server
	int			phase;			// shop phase the draft state belongs to
	qboolean	received;
	qboolean	logged;			// one time confirmation in the client log
	qboolean	hadCatcher;		// the window currently owns the mouse
	qboolean	dismissed;		// only the !draft command clears this
	int			cursor;			// keyboard cursor, -1 = nothing
	int			selected;		// hero the confirm button would act on
	int			scroll;			// first visible board row, for boards taller than the window
	char		owner[MOBA_MAX_HEROES][MAX_NETNAME];	// player name per taken hero, pushed by the server
	char		notice[64];
	int			noticeUntil;
} cgMobaDraft_t;

static cgMobaDraft_t cgMobaDraft;

//=========================================================================
// May this hero be the one the confirm button acts on? The server has the last
// word, this only decides whether the button lights up, and it is why a hero
// that just got banned or taken disappears from the button instead of failing
// with an error after the click.
//=========================================================================
static qboolean CG_Moba_Selectable( int heroId )
{
	unsigned long long pool;

	if ( heroId < 0 || heroId >= MOBA_MAX_HEROES || !cgMobaDraft.canAct ||
		cgMobaDraft.action == CG_MOBA_ACT_NONE )
	{
		return qfalse;
	}

	if ( cgMobaDraft.bannedMask & ( 1ULL << heroId ) )
	{
		return qfalse;
	}

	if ( cgMobaDraft.takenMask & ( 1ULL << heroId ) )
	{
		return qfalse;
	}

	// All pick has no pools at all: the whole board is up for grabs and the hero
	// leaves it for everybody as soon as one player owns it, which is what the
	// taken mask above already says.
	if ( cgMoba.phase == CG_MOBA_PHASE_DRAFT &&
		cgMobaDraft.mode == CG_MOBA_MODE_ALLPICK )
	{
		return qtrue;
	}

	if ( cgMoba.phase == CG_MOBA_PHASE_DRAFT )
	{
		unsigned long long gone;

		// the captains take heroes off the board, so a hero is gone as soon as it
		// sits in one of the two masks. That is a union, not an intersection: a
		// hero only one captain has taken must not be offered to the other.
		gone = cgMobaDraft.redMask | cgMobaDraft.blueMask;

		return ( gone & ( 1ULL << heroId ) ) ? qfalse : qtrue;
	}

	// the assign stage hands out heroes out of the pool of the own team
	pool = ( cgMobaDraft.myTeam == 1 ) ? cgMobaDraft.redMask :
		( cgMobaDraft.myTeam == 2 ) ? cgMobaDraft.blueMask : 0;

	return ( pool & ( 1ULL << heroId ) ) ? qtrue : qfalse;
}

//=========================================================================
// Server command handler: "mobaDraft <bannedLo> <bannedHi> <redLo> <redHi>
// <blueLo> <blueHi> <takenLo> <takenHi> <action> <canAct> <seconds> <myHero>
// <myTeam> <step> <steps> <mode>". The four hero sets are 64 bit masks and
// travel as two halves each, so the board can hold far more than thirty heroes
// and the window never has to ask the server for a second opinion on what is
// already banned or picked. <taken> is what the team pool masks cannot say: who
// already has a hero out of the pool. <mode> says whether the board is a shared
// one (all pick) or the pools of the two captains.
//=========================================================================
void CG_Moba_DraftCommand_f( void )
{
	char buf[192], *p;
	int v[16], i;

	if ( cg_moba.integer == 0 || !CG_Argv( 1 ) || !CG_Argv( 1 )[0] )
	{
		return;
	}

	Q_strncpyz( buf, CG_Argv( 1 ), sizeof( buf ) );
	p = buf;
	for ( i = 0; i < 16; i++ )
	{
		v[i] = (int)strtoul( p, &p, 10 );
		while ( *p == ' ' )
		{
			p++;
		}
	}

	// The shop state carries the phase and arrives first (MOBA_PushShopState is
	// called before MOBA_PushDraftState), so a phase change can be seen here.
	// It is the only moment the window may open itself again: a push that only
	// ticks the clock down must not undo an ESC.
	if ( cgMoba.phase != cgMobaDraft.phase )
	{
		cgMobaDraft.phase = cgMoba.phase;
		cgMobaDraft.dismissed = qfalse;
		cgMobaDraft.cursor = -1;
		cgMobaDraft.selected = -1;
		cgMobaDraft.scroll = 0;

		// a new draft must not show the owner names of the last one
		memset( cgMobaDraft.owner, 0, sizeof( cgMobaDraft.owner ) );

		// A new phase always clears the choice, and the mouse bit decides how the
		// window is currently held: still set means it kept the mouse across the
		// phase change, clear means it has to take it again.
		cgMobaDraft.hadCatcher = ( trap->Key_GetCatcher() & KEYCATCH_CGAME ) ? qtrue : qfalse;
	}

	cgMobaDraft.bannedMask = (unsigned long long)(unsigned)v[0] |
		((unsigned long long)(unsigned)v[1] << 32);
	cgMobaDraft.redMask = (unsigned long long)(unsigned)v[2] |
		((unsigned long long)(unsigned)v[3] << 32);
	cgMobaDraft.blueMask = (unsigned long long)(unsigned)v[4] |
		((unsigned long long)(unsigned)v[5] << 32);
	cgMobaDraft.takenMask = (unsigned long long)(unsigned)v[6] |
		((unsigned long long)(unsigned)v[7] << 32);
	cgMobaDraft.action = v[8];
	cgMobaDraft.canAct = v[9];
	cgMobaDraft.secondsLeft = v[10];
	cgMobaDraft.receivedAt = cg.time;
	cgMobaDraft.myHero = v[11];
	cgMobaDraft.myTeam = v[12];
	cgMobaDraft.step = v[13];
	cgMobaDraft.steps = v[14];
	cgMobaDraft.mode = v[15];
	cgMobaDraft.received = qtrue;

	// a hero that the server just took away must not stay armed in the confirm
	// button, or a late click would send a command for a hero nobody can have
	if ( cgMobaDraft.selected >= 0 && !CG_Moba_Selectable( cgMobaDraft.selected ) )
	{
		cgMobaDraft.selected = -1;
	}

	if ( !cgMobaDraft.logged )
	{
		cgMobaDraft.logged = qtrue;
		trap->Print( va( "MOBA: draft state received: %s\n", buf ) );
	}
}

//=========================================================================
// Server command handler: "mobaDraftOwner <heroId> <player name>". One line per
// owner and only when a pair changed. The name is everything after the first
// space so it may contain spaces; it is only used for the tile of a hero that
// the local player does not own.
//=========================================================================
void CG_Moba_DraftOwnerCommand_f( void )
{
	const char *arg = CG_Argv( 1 );
	char *p;
	int heroId;

	if ( cg_moba.integer == 0 || !arg || !arg[0] )
	{
		return;
	}

	heroId = (int)strtol( arg, &p, 10 );

	if ( heroId < 0 || heroId >= MOBA_MAX_HEROES )
	{
		return;
	}

	while ( *p == ' ' )
	{
		p++;
	}

	Q_strncpyz( cgMobaDraft.owner[heroId], p, sizeof( cgMobaDraft.owner[heroId] ) );
}

//=========================================================================
// Safety net for "the window is gone and I cannot get it back". ESC can no
// longer close it, but the server command costs nothing and covers the cases
// the client cannot see, a cgame that was restarted with a stale catcher, a
// window the player lost on another machine state. Fired by the !draft chat
// command of the server.
//=========================================================================
void CG_Moba_DraftOpen_f( void )
{
	if ( !cg_moba.integer || !cgMobaDraft.received )
	{
		return;
	}

	// the window only exists during the two draft phases, reopening it anywhere
	// else would only print a promise the client cannot keep
	if ( cgMoba.phase != CG_MOBA_PHASE_DRAFT && cgMoba.phase != CG_MOBA_PHASE_DRAFT_ASSIGN )
	{
		return;
	}

	// nothing to do, and no log line either: the command may be bound to a key in
	// some setups and would otherwise fill the client log once a second
	if ( !cgMobaDraft.dismissed && ( trap->Key_GetCatcher() & KEYCATCH_CGAME ) )
	{
		return;
	}

	cgMobaDraft.dismissed = qfalse;
	cgMobaDraft.hadCatcher = qfalse;
	trap->Print( "MOBA: draft window reopened by request\n" );
}

//=========================================================================
// The window exists while the server is in one of the two draft phases and the
// player did not ask for it to be hidden.
//=========================================================================
static qboolean CG_Moba_DraftWanted( void )
{
	if ( !cg_moba.integer || !cgMoba.received || !cgMobaDraft.received || cgMobaDraft.dismissed )
	{
		return qfalse;
	}

	return ( cgMoba.phase == CG_MOBA_PHASE_DRAFT ||
		cgMoba.phase == CG_MOBA_PHASE_DRAFT_ASSIGN ) ? qtrue : qfalse;
}

// The shop window is drawn at the end of this file, next to the state it reads.
// Both windows want the mouse and draw a pointer, so the helpers that do that
// are announced here.
static qboolean CG_Moba_ShopOpen( void );

//=========================================================================
// The JKA client only hands key codes to the cgame while KEYCATCH_CGAME is set
// (cl_keys.cpp CL_KeyEvent) and only feeds mouse deltas to CG_MouseEvent under
// the same bit (cl_input.cpp IN_MouseMove). A window therefore has to hold that
// bit, and it has to let it go again when it closes.
//
// ESC is the one key the client eats itself: it never reaches the cgame at all,
// it sets KEYCATCH_UI and opens the in game menu (cl_keys.cpp, "escape is always
// handled special" and UI_InGameMenu). So the two windows read the same event
// from opposite ends - a KEYCATCH_CGAME bit that disappeared while the window
// still wanted it:
//
//   draft: the bit came back on a press of ESC. A draft step may not be left
//          half way and the server has no undo, so the window takes the bit
//          back on the next frame instead of reading it as a close request.
//   shop:  that press is the close request the player asked for, so the window
//          lets the bit go and the game menu opens as usual.
//
// The console key and Shift+ESC are handled before that rule in the client, so a
// player can always open the console and the mouse look stays as it was.
//=========================================================================
static void CG_Moba_Catcher( void )
{
	int catcher = trap->Key_GetCatcher();
	qboolean hasBit = ( catcher & KEYCATCH_CGAME ) ? qtrue : qfalse;
	qboolean draft = CG_Moba_DraftWanted();
	qboolean shop = CG_Moba_ShopOpen();

	if ( !cg_moba.integer || ( !draft && !shop ) )
	{
		if ( hasBit && ( cgMobaDraft.hadCatcher || cgMoba.hadCatcher ) )
		{
			trap->Key_SetCatcher( catcher & ~KEYCATCH_CGAME );
		}

		cgMobaDraft.hadCatcher = qfalse;
		cgMoba.hadCatcher = qfalse;
		return;
	}

	if ( !hasBit )
	{
		if ( shop && cgMoba.hadCatcher )
		{
			// ESC took the bit: the shop is done, the menu that came with it stays
			cgMoba.open = qfalse;
			cgMoba.hadCatcher = qfalse;
			return;
		}

		trap->Key_SetCatcher( catcher | KEYCATCH_CGAME );
	}

	cgMobaDraft.hadCatcher = draft;
	cgMoba.hadCatcher = shop;
}

//=========================================================================
// Which tile is the mouse on. The gaps between the tiles are not tiles, and
// the window is the only place the cursor can reach a hero.
//=========================================================================
static int CG_Moba_MaxScroll( void )
{
	int rows = ( MOBA_MAX_HEROES + CG_MOBA_COLS - 1 ) / CG_MOBA_COLS;

	if ( rows <= CG_MOBA_ROWS )
	{
		return 0;
	}
	return rows - CG_MOBA_ROWS;
}

static void CG_Moba_ClampScroll( void )
{
	int max = CG_Moba_MaxScroll();

	if ( cgMobaDraft.scroll > max )
	{
		cgMobaDraft.scroll = max;
	}
	if ( cgMobaDraft.scroll < 0 )
	{
		cgMobaDraft.scroll = 0;
	}
}

// Keeps the keyboard cursor on screen: moving it down past the last visible row
// has to pull the board with it, otherwise the cursor would leave the window.
static void CG_Moba_EnsureCursorVisible( void )
{
	int row;

	if ( cgMobaDraft.cursor < 0 )
	{
		return;
	}

	row = cgMobaDraft.cursor / CG_MOBA_COLS;
	if ( row < cgMobaDraft.scroll )
	{
		cgMobaDraft.scroll = row;
	}
	else if ( row >= cgMobaDraft.scroll + CG_MOBA_ROWS )
	{
		cgMobaDraft.scroll = row - CG_MOBA_ROWS + 1;
	}
	CG_Moba_ClampScroll();
}

static int CG_Moba_TileAt( float mx, float my )
{
	float fx, fy;
	int col, row, index;

	if ( mx < CG_MOBA_GRID_X || my < CG_MOBA_GRID_Y ||
		mx > CG_MOBA_GRID_X + CG_MOBA_COLS * ( CG_MOBA_TILE_W + CG_MOBA_TILE_GAP ) ||
		my > CG_MOBA_GRID_Y + CG_MOBA_ROWS * ( CG_MOBA_TILE_H + CG_MOBA_TILE_GAP ) )
	{
		return -1;
	}

	fx = mx - CG_MOBA_GRID_X;
	fy = my - CG_MOBA_GRID_Y;
	col = (int)( fx / ( CG_MOBA_TILE_W + CG_MOBA_TILE_GAP ) );
	row = (int)( fy / ( CG_MOBA_TILE_H + CG_MOBA_TILE_GAP ) );

	if ( fx - col * ( CG_MOBA_TILE_W + CG_MOBA_TILE_GAP ) > CG_MOBA_TILE_W ||
		fy - row * ( CG_MOBA_TILE_H + CG_MOBA_TILE_GAP ) > CG_MOBA_TILE_H )
	{
		return -1;
	}

	if ( col >= CG_MOBA_COLS || row >= CG_MOBA_ROWS )
	{
		return -1;
	}

	// the board can be taller than the window, so the visible row has to be
	// shifted by the scroll offset before it becomes a hero index
	index = ( row + cgMobaDraft.scroll ) * CG_MOBA_COLS + col;
	if ( index < 0 || index >= MOBA_MAX_HEROES )
	{
		return -1;
	}

	return index;
}

static const char *CG_Moba_HeroState( int heroId )
{
	if ( cgMobaDraft.bannedMask & ( 1ULL << heroId ) )
	{
		return "banned";
	}
	if ( cgMobaDraft.redMask & ( 1ULL << heroId ) )
	{
		return "red team";
	}
	if ( cgMobaDraft.blueMask & ( 1ULL << heroId ) )
	{
		return "blue team";
	}
	if ( cgMobaDraft.myHero == heroId )
	{
		return "yours";
	}
	// all pick has no pools to show, so the only thing left to say about a hero
	// is that somebody else already owns it
	if ( cgMoba.phase == CG_MOBA_PHASE_DRAFT &&
		cgMobaDraft.mode == CG_MOBA_MODE_ALLPICK &&
		( cgMobaDraft.takenMask & ( 1ULL << heroId ) ) )
	{
		return "taken";
	}

	return NULL;
}

// The role is the only grouping the heroes have, and the tile colour has to
// make the three groups readable at a glance.
static void CG_Moba_RoleColor( const char *role, vec4_t out )
{
	static const vec4_t tank		= { 0.30f, 0.55f, 0.85f, 1.0f };
	static const vec4_t mage		= { 0.65f, 0.40f, 0.85f, 1.0f };
	static const vec4_t carry		= { 0.90f, 0.60f, 0.20f, 1.0f };
	static const vec4_t healer		= { 0.30f, 0.80f, 0.45f, 1.0f };
	static const vec4_t assassin	= { 0.85f, 0.30f, 0.30f, 1.0f };
	static const vec4_t unknown	= { 0.70f, 0.70f, 0.70f, 1.0f };
	int c;

	if ( !Q_stricmp( role, "Tank" ) )
	{
		for ( c = 0; c < 4; c++ ) out[c] = tank[c];
	}
	else if ( !Q_stricmp( role, "Mage" ) )
	{
		for ( c = 0; c < 4; c++ ) out[c] = mage[c];
	}
	else if ( !Q_stricmp( role, "Carry" ) )
	{
		for ( c = 0; c < 4; c++ ) out[c] = carry[c];
	}
	else if ( !Q_stricmp( role, "Healer" ) )
	{
		for ( c = 0; c < 4; c++ ) out[c] = healer[c];
	}
	else if ( !Q_stricmp( role, "Assassin" ) )
	{
		for ( c = 0; c < 4; c++ ) out[c] = assassin[c];
	}
	else
	{
		for ( c = 0; c < 4; c++ ) out[c] = unknown[c];
	}
}

//=========================================================================
// Is the mouse on the confirm button?
//=========================================================================
static qboolean CG_Moba_ButtonAt( float mx, float my )
{
	return ( mx >= CG_MOBA_BTN_X && mx <= CG_MOBA_BTN_X + CG_MOBA_BTN_W &&
		my >= CG_MOBA_BTN_Y && my <= CG_MOBA_BTN_Y + CG_MOBA_BTN_H ) ? qtrue : qfalse;
}

//=========================================================================
// The mouse pointer. The engine has no in game cursor at all, cgs.activeCursor
// is set in CG_MouseEvent and then never drawn, and the system cursor is hidden
// while the game has the mouse, so a window that wants a pointer has to paint
// one itself. Built from plain rectangles: no shader to load, and it stays
// crisp because every edge lands on a whole pixel.
//
// The pointer belongs to both windows, so it is drawn for whichever one has the
// mouse right now.
//=========================================================================
static void CG_Moba_DrawCursor( void )
{
	static const vec4_t colorDark	= { 0.00f, 0.00f, 0.00f, 0.90f };
	static const vec4_t colorLight	= { 1.00f, 1.00f, 1.00f, 1.00f };
	float x = (float)cgs.cursorX;
	float y = (float)cgs.cursorY;
	int i;

	if ( !cg_moba.integer || !( CG_Moba_DraftWanted() || CG_Moba_ShopOpen() ) )
	{
		return;
	}

	if ( x < 0.0f || y < 0.0f || x > 640.0f || y > 480.0f )
	{
		return;
	}

	// a dark halo first, so the pointer stays readable over a light tile
	for ( i = 0; i < 13; i++ )
	{
		CG_FillRect( x - 1.0f, y + i - 1.0f, 15.0f - i, 2.0f, colorDark );
	}

	// the arrow: a vertical left edge and a diagonal that runs down to the left
	for ( i = 0; i < 12; i++ )
	{
		CG_FillRect( x, y + i, 12.0f - i, 1.0f, colorLight );
	}

	// the little tail below the tip
	CG_FillRect( x - 1.0f, y + 12.0f, 4.0f, 8.0f, colorDark );
	CG_FillRect( x, y + 12.0f, 2.0f, 8.0f, colorLight );
}

//=========================================================================
// Turns the confirm button into the chat command the player would have typed.
// The server owns every rule, this only avoids the pointless round trip for a
// hero that is plainly gone already.
//=========================================================================
static void CG_Moba_DraftAct( int heroId )
{
	if ( heroId < 0 || heroId >= MOBA_MAX_HEROES )
	{
		return;
	}

	if ( !cgMobaDraft.canAct )
	{
		// all pick has no turn order, so a player that cannot act has his hero
		// already and not a turn that has to come
		Q_strncpyz( cgMobaDraft.notice,
			( cgMoba.phase == CG_MOBA_PHASE_DRAFT &&
				cgMobaDraft.mode != CG_MOBA_MODE_ALLPICK ) ?
				"wait for your turn" : "you already have a hero",
			sizeof( cgMobaDraft.notice ) );
		cgMobaDraft.noticeUntil = cg.time + 2500;
		return;
	}

	if ( cgMobaDraft.bannedMask & ( 1ULL << heroId ) )
	{
		Q_strncpyz( cgMobaDraft.notice, "that hero is banned", sizeof( cgMobaDraft.notice ) );
		cgMobaDraft.noticeUntil = cg.time + 2500;
		return;
	}

	if ( cgMobaDraft.takenMask & ( 1ULL << heroId ) )
	{
		Q_strncpyz( cgMobaDraft.notice, "that hero is already taken", sizeof( cgMobaDraft.notice ) );
		cgMobaDraft.noticeUntil = cg.time + 2500;
		return;
	}

	if ( cgMoba.phase == CG_MOBA_PHASE_DRAFT &&
		( cgMobaDraft.redMask & ( 1ULL << heroId ) || cgMobaDraft.blueMask & ( 1ULL << heroId ) ) )
	{
		Q_strncpyz( cgMobaDraft.notice, "that hero is already picked", sizeof( cgMobaDraft.notice ) );
		cgMobaDraft.noticeUntil = cg.time + 2500;
		return;
	}

	if ( cgMobaDraft.action == CG_MOBA_ACT_NONE )
	{
		Q_strncpyz( cgMobaDraft.notice, "wait for the next step", sizeof( cgMobaDraft.notice ) );
		cgMobaDraft.noticeUntil = cg.time + 2500;
		return;
	}

	// the vm console command queue has no separator, the newline has to be part
	// of the command or it merges with the next one, and "cmd" makes the client
	// forward it to the server after the cgame and the ui had their turn
	trap->SendConsoleCommand( va( "cmd say !%s %i\n",
		( cgMobaDraft.action == CG_MOBA_ACT_BAN ) ? "ban" : "pick", heroId + 1 ) );

	// one decision per step, the hero is gone from the button as soon as the
	// command is on its way
	cgMobaDraft.selected = -1;
	cgMobaDraft.cursor = -1;
}

//=========================================================================
// The ability bar and the cast path live further down, next to the ability state
// they work on. The key handler needs them for the click on an icon, so they are
// announced here.
//=========================================================================
// The bar is a corner of the screen, not the middle of it: a quarter of the
// height of a box would cover the fight the player is trying to watch. The two
// item slots sit on the same row, left of the abilities.
#define CG_MOBA_BAR_SIZE		22.0f
#define CG_MOBA_BAR_GAP		4.0f
// high enough that a slot and the line above it both stay on screen
#define CG_MOBA_BAR_Y		( 480.0f - 29.0f )

static qboolean CG_Moba_BarWanted( void );
static int CG_Moba_BarAt( float mx, float my );
static void CG_Moba_CastAbility( int slot );

// The two item slots work the same way: the key handler needs to know whether
// the mouse is on one of them and has to be able to send the use for it.
static float CG_Moba_SlotX( int slot );
static qboolean CG_Moba_SlotsWanted( void );
static void CG_Moba_UseSlot( int slot );

//=========================================================================
// The shop window is a window like the draft, so it has its own click handling.
// A purchase spends gold the player cannot get back this phase, but the server
// is the one that decides: the click only sends the !buy a player would type,
// and the tile is only lit when the server would accept it right now.
//=========================================================================
static int CG_Moba_ShopTabAt( float mx, float my );
static int CG_Moba_ShopTileAt( float mx, float my );
static void CG_Moba_ShopAct( int item );
static qboolean CG_Moba_ShopBuyable( int item );
static void CG_Moba_ShopMoveCursor( int dx, int dy );

//=========================================================================
// Input. Runs before the cgame decides what a key is for, because a living
// local player would otherwise swallow every key and every mouse button.
// Returns qtrue when the window used the key, so the game never acts on it.
//
// A left click only arms the confirm button. A draft step may not be undone and
// the server has no take back, so the click that decides has to be a second,
// separate click on the button.
//=========================================================================
// which item slot the mouse is over, -1 when it is nowhere near the two boxes
static int CG_Moba_SlotAt( float mx, float my )
{
	int i;

	for ( i = 0; i < 2; i++ )
	{
		float x = CG_Moba_SlotX( i );

		if ( mx >= x && mx < x + CG_MOBA_BAR_SIZE &&
			my >= CG_MOBA_BAR_Y && my < CG_MOBA_BAR_Y + CG_MOBA_BAR_SIZE )
		{
			return i;
		}
	}

	return -1;
}

qboolean CG_Moba_KeyEvent( int key, qboolean down )
{
	int hero, slot, item, tab;

	if ( !down )
	{
		return qfalse;
	}

	// The ability bar is on screen in the fight, and a left click on one of its
	// icons is a second way to cast. The key is consumed so the same click does
	// not also swing the saber.
	if ( CG_Moba_BarWanted() )
	{
		slot = CG_Moba_BarAt( (float)cgs.cursorX, (float)cgs.cursorY );

		if ( slot >= 0 && key == A_MOUSE1 )
		{
			CG_Moba_CastAbility( slot );
			return qtrue;
		}
	}

	// The same for the two item slots: a left click is a second way to use the
	// item, which is worth having in a fight.
	if ( CG_Moba_SlotsWanted() )
	{
		slot = CG_Moba_SlotAt( (float)cgs.cursorX, (float)cgs.cursorY );

		if ( slot >= 0 && key == A_MOUSE1 )
		{
			CG_Moba_UseSlot( slot );
			return qtrue;
		}
	}

	// The shop has the mouse, so every click belongs to it. ESC never gets here
	// (the client eats it), CG_Moba_Catcher reads the closed shop out of the
	// catcher bit the client took away.
	if ( CG_Moba_ShopOpen() )
	{
		switch ( key )
		{
		case A_MOUSE1:
			tab = CG_Moba_ShopTabAt( (float)cgs.cursorX, (float)cgs.cursorY );
			if ( tab >= 0 )
			{
				cgMoba.category = tab;
			}
			else
			{
				item = CG_Moba_ShopTileAt( (float)cgs.cursorX, (float)cgs.cursorY );
				CG_Moba_ShopAct( item );
			}
			return qtrue;

		case A_CURSOR_LEFT:
			CG_Moba_ShopMoveCursor( -1, 0 );
			return qtrue;

		case A_CURSOR_RIGHT:
			CG_Moba_ShopMoveCursor( 1, 0 );
			return qtrue;

		case A_CURSOR_UP:
			CG_Moba_ShopMoveCursor( 0, -1 );
			return qtrue;

		case A_CURSOR_DOWN:
			CG_Moba_ShopMoveCursor( 0, 1 );
			return qtrue;

		case A_ENTER:
			CG_Moba_ShopAct( CG_Moba_ShopTileAt( (float)cgs.cursorX, (float)cgs.cursorY ) );
			return qtrue;
		}

		return qtrue;
	}

	if ( !CG_Moba_DraftWanted() )
	{
		return qfalse;
	}

	hero = CG_Moba_TileAt( (float)cgs.cursorX, (float)cgs.cursorY );

	switch ( key )
	{
	case A_MOUSE1:
		if ( CG_Moba_ButtonAt( (float)cgs.cursorX, (float)cgs.cursorY ) )
		{
			CG_Moba_DraftAct( cgMobaDraft.selected );
		}
		else if ( hero >= 0 )
		{
			if ( !CG_Moba_Selectable( hero ) )
			{
				cgMobaDraft.selected = -1;
				CG_Moba_DraftAct( hero );	// only to raise the notice
			}
			else
			{
				cgMobaDraft.selected = hero;
				cgMobaDraft.cursor = hero;
			}
		}
		return qtrue;

	case A_ENTER:
		CG_Moba_DraftAct( ( cgMobaDraft.selected >= 0 ) ? cgMobaDraft.selected : cgMobaDraft.cursor );
		return qtrue;

	case A_CURSOR_LEFT:
		if ( cgMobaDraft.cursor < 0 )
		{
			cgMobaDraft.cursor = 0;
		}
		else if ( ( cgMobaDraft.cursor % CG_MOBA_COLS ) > 0 )
		{
			cgMobaDraft.cursor--;
		}
		cgMobaDraft.selected = cgMobaDraft.cursor;
		CG_Moba_EnsureCursorVisible();
		return qtrue;

	case A_CURSOR_RIGHT:
		if ( cgMobaDraft.cursor < 0 )
		{
			cgMobaDraft.cursor = 0;
		}
		else if ( ( cgMobaDraft.cursor % CG_MOBA_COLS ) < CG_MOBA_COLS - 1 )
		{
			cgMobaDraft.cursor++;
		}
		cgMobaDraft.selected = cgMobaDraft.cursor;
		CG_Moba_EnsureCursorVisible();
		return qtrue;

	case A_CURSOR_UP:
		if ( cgMobaDraft.cursor < 0 )
		{
			cgMobaDraft.cursor = 0;
		}
		else if ( cgMobaDraft.cursor >= CG_MOBA_COLS )
		{
			cgMobaDraft.cursor -= CG_MOBA_COLS;
		}
		cgMobaDraft.selected = cgMobaDraft.cursor;
		CG_Moba_EnsureCursorVisible();
		return qtrue;

	case A_CURSOR_DOWN:
		if ( cgMobaDraft.cursor < 0 )
		{
			cgMobaDraft.cursor = 0;
		}
		else if ( cgMobaDraft.cursor + CG_MOBA_COLS < MOBA_MAX_HEROES )
		{
			cgMobaDraft.cursor += CG_MOBA_COLS;
		}
		cgMobaDraft.selected = cgMobaDraft.cursor;
		CG_Moba_EnsureCursorVisible();
		return qtrue;

	// the mouse wheel scrolls the board the same way it scrolls a list, and the
	// scrollbar is drawn from the same offset
	case A_MWHEELUP:
		if ( cgMobaDraft.scroll > 0 )
		{
			cgMobaDraft.scroll--;
		}
		return qtrue;

	case A_MWHEELDOWN:
		if ( cgMobaDraft.scroll < CG_Moba_MaxScroll() )
		{
			cgMobaDraft.scroll++;
		}
		return qtrue;

	case A_0:
		cgMobaDraft.selected = ( CG_Moba_Selectable( 9 ) ) ? 9 : -1;
		if ( cgMobaDraft.selected < 0 )
		{
			CG_Moba_DraftAct( 9 );
		}
		return qtrue;

	case A_1: case A_2: case A_3: case A_4: case A_5:
	case A_6: case A_7: case A_8: case A_9:
		// the number on the tile is the number of !pick, so the keys and the
		// board can never mean different heroes
		hero = key - A_1;
		if ( !CG_Moba_Selectable( hero ) )
		{
			cgMobaDraft.selected = -1;
			CG_Moba_DraftAct( hero );		// only to raise the notice
		}
		else
		{
			cgMobaDraft.selected = hero;
			cgMobaDraft.cursor = hero;
			CG_Moba_EnsureCursorVisible();
		}
		return qtrue;
	}

	return qfalse;
}

static const char *CG_Moba_AbilityTag( const mobaAbility_t *ab )
{
	switch ( ab->type )
	{
	case AB_AOE_DAMAGE:	return "area";
	case AB_AOE_HEAL:		return "heal";
	case AB_BUFF:			return "buff";
	case AB_LEAP:			return "leap";
	case AB_SHIELD:			return "shield";
	case AB_PROJECTILE:		return "shot";
	case AB_FLAME:			return "flame";
	case AB_SILENCE:		return "silence";
	case AB_MAGICRESIST:	return "mag.res";
	default:				return "hit";
	}
}

static const char *CG_Moba_AbilityEffect( const mobaAbility_t *ab )
{
	if ( ab->type == AB_BUFF )
	{
		return va( "+%i%% damage for %i s",
			(int)( ( ab->buffMult - 1.0f ) * 100.0f ), ab->durationMs / 1000 );
	}

	if ( ab->type == AB_AOE_HEAL )
	{
		return va( "heal %i (+%i)  radius %i", ab->baseEffect, ab->perLevelEffect,
			(int)ab->radius );
	}

	if ( ab->type == AB_AOE_DAMAGE )
	{
		return va( "damage %i (+%i)  radius %i", ab->baseEffect, ab->perLevelEffect,
			(int)ab->radius );
	}

	if ( ab->type == AB_LEAP )
	{
		return va( "jump %i  slow %i%% for %i s", (int)ab->range,
			(int)( ab->buffMult * 100.0f ), ab->durationMs / 1000 );
	}

	if ( ab->type == AB_SHIELD )
	{
		return va( "shield %i (+%i)  for %i s", ab->baseEffect, ab->perLevelEffect,
			ab->durationMs / 1000 );
	}

	if ( ab->type == AB_PROJECTILE )
	{
		return va( "bolt %i (+%i)  4 charges", ab->baseEffect, ab->perLevelEffect );
	}

	if ( ab->type == AB_FLAME )
	{
		return va( "burn %i/s  range %i", ab->baseEffect, (int)ab->range );
	}

	if ( ab->type == AB_SILENCE )
	{
		return va( "silence %i s  radius %i", ab->durationMs / 1000, (int)ab->radius );
	}

	if ( ab->type == AB_MAGICRESIST )
	{
		return va( "resist %i%%  for %i s", (int)( ab->buffMult * 100.0f ),
			ab->durationMs / 1000 );
	}

	return va( "damage %i (+%i)  range %i", ab->baseEffect, ab->perLevelEffect,
		(int)ab->range );
}

//=========================================================================
// The ability bar and its icons are further down, next to the ability state
// they draw. The hero window needs the same icons and the same key letters, so
// it says up front what it borrows from there.
//=========================================================================
static const char *cgMobaAbilityKeys[CG_MOBA_ABILITIES];
static void CG_Moba_AbilityIcon( float x, float y, float s, const mobaAbility_t *ab, float alpha );

//=========================================================================
// Draws the hero select window: every hero of the shared table on the left,
// the details of the hero under the mouse on the right, the bans and the team
// picks on the tiles themselves so the board is readable without any chat.
//=========================================================================
//=========================================================================
// Draws a hero name inside a tile. A name wider than the tile wraps at a space
// into two lines, a name without a usable space is cut down to the tile so it
// never spills over its neighbours.
//=========================================================================
#define CG_MOBA_NAME_SCALE	0.62f
#define CG_MOBA_NAME_GAP	8.0f

static void CG_Moba_DrawTileName( float x, float y, vec4_t color, const char *name )
{
	float maxW = CG_MOBA_TILE_W - 8.0f;
	float w = (float)CG_Text_Width( name, CG_MOBA_NAME_SCALE, FONT_SMALL );
	char left[64], right[64];
	const char *best = NULL;
	int i, len;
	float bestMax = 0.0f;

	if ( w <= maxW )
	{
		CG_Text_Paint( x + ( CG_MOBA_TILE_W - w ) * 0.5f, y, CG_MOBA_NAME_SCALE, color,
			name, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		return;
	}

	len = (int)strlen( name );

	// look for the space that leaves the two halves closest in width, so the
	// break lands in the middle of the name instead of after the first word
	for ( i = 1; i < len - 1; i++ )
	{
		float lw, rw, m;

		if ( name[i] != ' ' )
		{
			continue;
		}

		if ( i > (int)sizeof( left ) - 1 )
		{
			break;
		}

		memcpy( left, name, i );
		left[i] = '\0';

		lw = (float)CG_Text_Width( left, CG_MOBA_NAME_SCALE, FONT_SMALL );
		rw = (float)CG_Text_Width( name + i + 1, CG_MOBA_NAME_SCALE, FONT_SMALL );
		m = ( lw > rw ) ? lw : rw;

		if ( !best || m < bestMax )
		{
			best = name + i;
			bestMax = m;
		}
	}

	if ( best && bestMax <= maxW )
	{
		int n = (int)( best - name );

		memcpy( left, name, n );
		left[n] = '\0';
		Q_strncpyz( right, best + 1, sizeof( right ) );

		w = (float)CG_Text_Width( left, CG_MOBA_NAME_SCALE, FONT_SMALL );
		CG_Text_Paint( x + ( CG_MOBA_TILE_W - w ) * 0.5f, y, CG_MOBA_NAME_SCALE, color,
			left, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		w = (float)CG_Text_Width( right, CG_MOBA_NAME_SCALE, FONT_SMALL );
		CG_Text_Paint( x + ( CG_MOBA_TILE_W - w ) * 0.5f, y + CG_MOBA_NAME_GAP,
			CG_MOBA_NAME_SCALE, color, right, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		return;
	}

	// no space that helps: cut the single line down to the tile
	{
		int n = len;

		if ( n > (int)sizeof( left ) - 1 )
		{
			n = (int)sizeof( left ) - 1;
		}
		memcpy( left, name, n );
		left[n] = '\0';

		while ( n > 1 && CG_Text_Width( left, CG_MOBA_NAME_SCALE, FONT_SMALL ) > maxW )
		{
			left[--n] = '\0';
		}

		w = (float)CG_Text_Width( left, CG_MOBA_NAME_SCALE, FONT_SMALL );
		CG_Text_Paint( x + ( CG_MOBA_TILE_W - w ) * 0.5f, y, CG_MOBA_NAME_SCALE, color,
			left, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}
}

void CG_Moba_DrawDraft( void )
{
	static vec4_t colorWindow		= { 0.02f, 0.02f, 0.05f, 0.90f };
	static vec4_t colorBorder		= { 0.60f, 0.50f, 0.20f, 0.95f };
	static vec4_t colorTitle		= { 1.00f, 0.85f, 0.30f, 1.00f };
	static vec4_t colorText			= { 1.00f, 1.00f, 1.00f, 1.00f };
	static vec4_t colorDim			= { 0.65f, 0.65f, 0.65f, 1.00f };
	static vec4_t colorHint			= { 0.60f, 0.60f, 0.60f, 1.00f };
	static vec4_t colorRed			= { 0.90f, 0.20f, 0.20f, 1.00f };
	static vec4_t colorBlue			= { 0.30f, 0.55f, 1.00f, 1.00f };
	static vec4_t colorGold			= { 1.00f, 0.80f, 0.25f, 1.00f };
	static vec4_t colorGreen		= { 0.40f, 0.95f, 0.45f, 1.00f };
	static vec4_t colorBannedText	= { 0.55f, 0.45f, 0.45f, 0.45f };
	static vec4_t colorTileBg		= { 0.10f, 0.10f, 0.14f, 0.85f };
	static vec4_t colorHover		= { 1.00f, 1.00f, 1.00f, 0.95f };
	static vec4_t colorSelected	= { 0.20f, 1.00f, 0.35f, 1.00f };
	static vec4_t colorBtnOn		= { 0.15f, 0.45f, 0.20f, 0.95f };
	static vec4_t colorBtnOff		= { 0.16f, 0.16f, 0.18f, 0.95f };
	static vec4_t colorCursorDark	= { 0.00f, 0.00f, 0.00f, 0.85f };
	static vec4_t colorCursorLight	= { 1.00f, 1.00f, 1.00f, 1.00f };

	vec4_t roleColor, bg, border;
	float x, y, textY;
	int i, col, row, secondsLeft, hover, active, btnReady;
	const char *state;

	// the catcher has to be serviced every frame, also while the window is
	// closed, otherwise the mouse would stay locked in a panel nobody can see
	CG_Moba_Catcher();

	if ( !CG_Moba_DraftWanted() )
	{
		return;
	}

	CG_FillRect( CG_MOBA_WIN_X, CG_MOBA_WIN_Y, CG_MOBA_WIN_W, CG_MOBA_WIN_H, colorWindow );
	CG_DrawRect( CG_MOBA_WIN_X, CG_MOBA_WIN_Y, CG_MOBA_WIN_W, CG_MOBA_WIN_H, 1.0f, colorBorder );

	// ---- title bar ----
	CG_Text_Paint( CG_MOBA_WIN_X + 8.0f, CG_MOBA_WIN_Y + 4.0f, 0.8f, colorTitle,
		( cgMoba.phase == CG_MOBA_PHASE_DRAFT_ASSIGN ) ? "CHOOSE YOUR HERO" :
		( cgMobaDraft.mode == CG_MOBA_MODE_ALLPICK ) ? "ALL PICK" : "CAPTAIN DRAFT",
		0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_MEDIUM );

	secondsLeft = cgMobaDraft.secondsLeft - ( cg.time - cgMobaDraft.receivedAt ) / 1000;
	if ( secondsLeft < 0 )
	{
		secondsLeft = 0;
	}

	if ( cgMoba.phase == CG_MOBA_PHASE_DRAFT &&
		cgMobaDraft.mode == CG_MOBA_MODE_ALLPICK )
	{
		// all pick has no steps and no turn order, only the clock and the board
		textY = CG_MOBA_WIN_Y + 20.0f;
		CG_Text_Paint( CG_MOBA_WIN_X + 8.0f, textY, 0.62f,
			( secondsLeft <= 5 ) ? colorRed : colorDim,
			va( "pick any hero you like  -  %i s", secondsLeft ),
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		CG_Text_Paint( CG_MOBA_WIN_X + 8.0f, textY + 12.0f, 0.62f,
			cgMobaDraft.canAct ? colorGold : colorDim,
			cgMobaDraft.canAct ? "click a free hero - no captain in between" :
				( cgMobaDraft.myHero >= 0 ? "you already picked a hero" : "waiting" ),
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}
	else if ( cgMoba.phase == CG_MOBA_PHASE_DRAFT )
	{
		textY = CG_MOBA_WIN_Y + 20.0f;
		CG_Text_Paint( CG_MOBA_WIN_X + 8.0f, textY, 0.62f,
			( secondsLeft <= 5 ) ? colorRed : colorDim,
			va( "step %i of %i  -  %i s", cgMobaDraft.step + 1, cgMobaDraft.steps, secondsLeft ),
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		CG_Text_Paint( CG_MOBA_WIN_X + 8.0f, textY + 12.0f, 0.62f,
			cgMobaDraft.canAct ? colorGold : colorDim,
			cgMobaDraft.canAct ?
				( ( cgMobaDraft.action == CG_MOBA_ACT_BAN ) ? "your turn - ban a hero" : "your turn - pick a hero" ) :
				"the captains are picking",
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}
	else
	{
		textY = CG_MOBA_WIN_Y + 20.0f;
		CG_Text_Paint( CG_MOBA_WIN_X + 8.0f, textY, 0.62f,
			( secondsLeft <= 5 ) ? colorRed : colorDim,
			va( "take a hero from your team pool  -  %i s", secondsLeft ),
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		CG_Text_Paint( CG_MOBA_WIN_X + 8.0f, textY + 12.0f, 0.62f,
			cgMobaDraft.canAct ? colorGold : colorDim,
			cgMobaDraft.canAct ? "click a hero of your team" : "waiting",
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}

	// ---- the board ----
	hover = CG_Moba_TileAt( (float)cgs.cursorX, (float)cgs.cursorY );
	active = ( hover >= 0 ) ? hover :
		( cgMobaDraft.selected >= 0 ? cgMobaDraft.selected : cgMobaDraft.cursor );

	for ( i = 0; i < MOBA_MAX_HEROES; i++ )
	{
		const mobaHero_t *hero = &mobaHeroTable[i];
		qboolean banned = ( cgMobaDraft.bannedMask & ( 1ULL << i ) ) ? qtrue : qfalse;
		qboolean red = ( cgMobaDraft.redMask & ( 1ULL << i ) ) ? qtrue : qfalse;
		qboolean blue = ( cgMobaDraft.blueMask & ( 1ULL << i ) ) ? qtrue : qfalse;
		qboolean showOwner;
		char nameBuf[MAX_NETNAME];
		int c;

		col = i % CG_MOBA_COLS;
		row = i / CG_MOBA_COLS - cgMobaDraft.scroll;

		// the board can hold more heroes than the window has rows, the rows
		// outside the window are simply not drawn
		if ( row < 0 || row >= CG_MOBA_ROWS )
		{
			continue;
		}

		x = CG_MOBA_GRID_X + col * ( CG_MOBA_TILE_W + CG_MOBA_TILE_GAP );
		y = CG_MOBA_GRID_Y + row * ( CG_MOBA_TILE_H + CG_MOBA_TILE_GAP );

		CG_Moba_RoleColor( hero->role, roleColor );

		for ( c = 0; c < 4; c++ )
		{
			bg[c] = banned ? 0.05f : colorTileBg[c];
			border[c] = roleColor[c];
		}
		bg[0] = banned ? 0.05f : roleColor[0] * 0.35f;
		bg[1] = banned ? 0.04f : roleColor[1] * 0.35f;
		bg[2] = banned ? 0.06f : roleColor[2] * 0.35f;

		CG_FillRect( x, y, CG_MOBA_TILE_W, CG_MOBA_TILE_H, bg );

		// a banned hero stays in the board but is pushed into the background,
		// a picked one wears the colour of the team that took him
		border[3] = banned ? 0.30f : 0.85f;
		CG_DrawRect( x, y, CG_MOBA_TILE_W, CG_MOBA_TILE_H, 1.0f, border );

		if ( red || blue )
		{
			CG_DrawRect( x + 1.0f, y + 1.0f, CG_MOBA_TILE_W - 2.0f, CG_MOBA_TILE_H - 2.0f,
				2.0f, red ? colorRed : colorBlue );
		}

		if ( cgMobaDraft.myHero == i )
		{
			CG_DrawRect( x + 1.0f, y + 1.0f, CG_MOBA_TILE_W - 2.0f, CG_MOBA_TILE_H - 2.0f,
				2.0f, colorGold );
		}

		if ( i == active )
		{
			CG_DrawRect( x + 1.0f, y + 1.0f, CG_MOBA_TILE_W - 2.0f, CG_MOBA_TILE_H - 2.0f,
				2.0f, colorHover );
		}

		// the hero the confirm button would act on is the one that matters, and
		// a thick green frame reads as "armed" even in the corner of the eye
		if ( cgMobaDraft.selected == i && CG_Moba_Selectable( i ) )
		{
			CG_DrawRect( x + 1.0f, y + 1.0f, CG_MOBA_TILE_W - 2.0f, CG_MOBA_TILE_H - 2.0f,
				3.0f, colorSelected );
		}

		CG_Text_Paint( x + 4.0f, y + 2.0f, 0.55f, banned ? colorBannedText : colorDim,
			va( "%i", i + 1 ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		CG_Moba_DrawTileName( x, y + 14.0f, banned ? colorBannedText : colorText, hero->name );

		CG_Text_Paint( x + 5.0f, y + 32.0f, 0.55f, banned ? colorBannedText : roleColor,
			hero->role, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		state = CG_Moba_HeroState( i );

		// A hero somebody else owns is labelled with the owner's name instead of
		// the generic "you", on every client. The name takes the role line and is
		// cut down to the tile so it never spills into the neighbour. The owner
		// himself keeps "you" in the corner.
		showOwner = ( state && !banned && !red && !blue &&
			cgMobaDraft.myHero != i && cgMobaDraft.owner[i][0] ) ? qtrue : qfalse;

		if ( showOwner )
		{
			int len = (int)strlen( cgMobaDraft.owner[i] );

			if ( len > (int)sizeof( nameBuf ) - 1 )
			{
				len = (int)sizeof( nameBuf ) - 1;
			}
			memcpy( nameBuf, cgMobaDraft.owner[i], len );
			nameBuf[len] = '\0';

			while ( len > 1 && CG_Text_Width( nameBuf, 0.55f, FONT_SMALL ) >
				CG_MOBA_TILE_W - 10.0f )
			{
				nameBuf[--len] = '\0';
			}

			// the role is dropped for a taken hero, the owner's name is the more
			// useful thing to read on that tile
			CG_FillRect( x + 4.0f, y + 26.0f, CG_MOBA_TILE_W - 8.0f, 10.0f, bg );
			CG_Text_Paint( x + 5.0f, y + 32.0f, 0.55f, colorGold, nameBuf,
				0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}

		if ( banned )
		{
			CG_Text_Paint( x + CG_MOBA_TILE_W - 26.0f, y + 32.0f, 0.55f, colorBannedText,
				"ban", 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}
		else if ( red || blue )
		{
			CG_Text_Paint( x + CG_MOBA_TILE_W - 26.0f, y + 32.0f, 0.55f,
				red ? colorRed : colorBlue, red ? "red" : "blue",
				0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}
		else if ( state && !showOwner )
		{
			CG_Text_Paint( x + CG_MOBA_TILE_W - 26.0f, y + 32.0f, 0.55f, colorGold,
				"you", 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}
	}

	// ---- scrollbar ----
	// The board scrolls only while it holds more rows than the window shows, so
	// the bar appears exactly then and its thumb says how far down the player is.
	if ( CG_Moba_MaxScroll() > 0 )
	{
		float trackX = CG_MOBA_GRID_X + CG_MOBA_COLS * ( CG_MOBA_TILE_W + CG_MOBA_TILE_GAP ) + 1.0f;
		float trackY = CG_MOBA_GRID_Y;
		float trackH = CG_MOBA_ROWS * ( CG_MOBA_TILE_H + CG_MOBA_TILE_GAP ) - CG_MOBA_TILE_GAP;
		int totalRows = ( MOBA_MAX_HEROES + CG_MOBA_COLS - 1 ) / CG_MOBA_COLS;
		float thumbH = trackH * (float)CG_MOBA_ROWS / (float)totalRows;
		float thumbY = trackY + ( trackH - thumbH ) * (float)cgMobaDraft.scroll / (float)CG_Moba_MaxScroll();

		CG_FillRect( trackX, trackY, 4.0f, trackH, colorBtnOff );
		CG_DrawRect( trackX, trackY, 4.0f, trackH, 1.0f, colorBorder );
		CG_FillRect( trackX, thumbY, 4.0f, thumbH, colorBorder );
	}

	// ---- the confirm button ----
	// A left click on a tile only arms it, this is the click that decides. The
	// server has no take back, so the step may not be lost to a stray click.
	btnReady = CG_Moba_Selectable( cgMobaDraft.selected ) ? 1 : 0;

	CG_FillRect( CG_MOBA_BTN_X, CG_MOBA_BTN_Y, CG_MOBA_BTN_W, CG_MOBA_BTN_H,
		btnReady ? colorBtnOn : colorBtnOff );
	CG_DrawRect( CG_MOBA_BTN_X, CG_MOBA_BTN_Y, CG_MOBA_BTN_W, CG_MOBA_BTN_H, 1.0f, colorBorder );
	CG_DrawRect( CG_MOBA_BTN_X, CG_MOBA_BTN_Y, CG_MOBA_BTN_W, CG_MOBA_BTN_H, 1.0f,
		btnReady ? colorSelected : colorDim );

	if ( CG_Moba_ButtonAt( (float)cgs.cursorX, (float)cgs.cursorY ) && btnReady )
	{
		CG_DrawRect( CG_MOBA_BTN_X + 1.0f, CG_MOBA_BTN_Y + 1.0f,
			CG_MOBA_BTN_W - 2.0f, CG_MOBA_BTN_H - 2.0f, 2.0f, colorSelected );
	}

	if ( cgMobaDraft.selected >= 0 )
	{
		CG_Text_Paint( CG_MOBA_BTN_X + CG_MOBA_BTN_W * 0.5f -
			CG_Text_Width( "CONFIRM", 0.66f, FONT_SMALL ) * 0.5f,
			CG_MOBA_BTN_Y + 4.0f, 0.66f, btnReady ? colorText : colorDim,
			"CONFIRM", 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}
	else
	{
		CG_Text_Paint( CG_MOBA_BTN_X + CG_MOBA_BTN_W * 0.5f -
			CG_Text_Width( "pick a hero first", 0.62f, FONT_SMALL ) * 0.5f,
			CG_MOBA_BTN_Y + 5.0f, 0.62f, colorDim,
			"pick a hero first", 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}

	// what the button would do, and why a click on a tile did nothing
	x = CG_MOBA_BTN_X + CG_MOBA_BTN_W + 10.0f;
	if ( cg.time < cgMobaDraft.noticeUntil && cgMobaDraft.notice[0] )
	{
		CG_Text_Paint( x, CG_MOBA_BTN_Y + 4.0f, 0.64f, colorGold,
			cgMobaDraft.notice, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}
	else if ( cgMobaDraft.selected >= 0 && btnReady )
	{
		CG_Text_Paint( x, CG_MOBA_BTN_Y + 4.0f, 0.64f, colorText,
			va( "%s %s", ( cgMobaDraft.action == CG_MOBA_ACT_BAN ) ? "ban" : "take",
				mobaHeroTable[cgMobaDraft.selected].name ),
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}
	else if ( !cgMobaDraft.canAct )
	{
		CG_Text_Paint( x, CG_MOBA_BTN_Y + 4.0f, 0.6f, colorHint,
			( cgMoba.phase == CG_MOBA_PHASE_DRAFT ) ?
				"the captains are picking, wait for your turn" : "you already have a hero",
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}
	else
	{
		CG_Text_Paint( x, CG_MOBA_BTN_Y + 4.0f, 0.6f, colorHint,
			( cgMoba.phase == CG_MOBA_PHASE_DRAFT ) ?
				"click a hero, then confirm  -  ESC does not close the window" :
				"click a hero of your team, then confirm  -  !draft brings the window back",
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}

	// ---- details of the hero under the mouse ----
	CG_FillRect( CG_MOBA_INFO_X, CG_MOBA_GRID_Y, CG_MOBA_INFO_W,
		CG_MOBA_ROWS * ( CG_MOBA_TILE_H + CG_MOBA_TILE_GAP ) - CG_MOBA_TILE_GAP,
		colorTileBg );
	CG_DrawRect( CG_MOBA_INFO_X, CG_MOBA_GRID_Y, CG_MOBA_INFO_W,
		CG_MOBA_ROWS * ( CG_MOBA_TILE_H + CG_MOBA_TILE_GAP ) - CG_MOBA_TILE_GAP,
		1.0f, colorBorder );

	x = CG_MOBA_INFO_X + 6.0f;
	textY = CG_MOBA_GRID_Y + 4.0f;

	if ( active < 0 || active >= MOBA_MAX_HEROES )
	{
		CG_Text_Paint( x, textY, 0.62f, colorHint, "point at a hero", 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		CG_Text_Paint( x, textY + 12.0f, 0.62f, colorHint, "for his numbers", 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		return;
	}

	{
		const mobaHero_t *hero = &mobaHeroTable[active];
		int a;

		CG_Text_Paint( x, textY, 0.78f, colorTitle, hero->name, 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_MEDIUM );
		textY += 15.0f;

		CG_Moba_RoleColor( hero->role, roleColor );
		CG_Text_Paint( x, textY, 0.6f, roleColor, hero->role, 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		textY += 12.0f;

		CG_Text_Paint( x, textY, 0.58f, colorText, va( "Weapon: %s", hero->weapons ), 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		textY += 13.0f;

		CG_Text_Paint( x, textY, 0.58f, colorGreen,
			va( "Health %i (+%i/lv)", hero->baseHealth, hero->healthPerLevel ), 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		textY += 12.0f;

		CG_Text_Paint( x, textY, 0.58f, colorDim,
			va( "Armor %i", hero->baseArmor ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		textY += 12.0f;

		CG_Text_Paint( x, textY, 0.58f, colorGreen,
			va( "Mana %i", hero->maxMana ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		textY += 12.0f;

		CG_Text_Paint( x, textY, 0.58f, colorDim,
			va( "Damage %i (+%i/lv)", hero->baseDamage, hero->damagePerLevel ), 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		textY += 12.0f;

		state = CG_Moba_HeroState( active );
		if ( state )
		{
			CG_Text_Paint( x, textY, 0.58f,
				( cgMobaDraft.bannedMask & ( 1ULL << active ) ) ? colorRed : colorDim,
				state, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
			textY += 12.0f;
		}

		textY += 4.0f;
		CG_Text_Paint( x, textY, 0.6f, colorTitle, "ABILITIES", 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		textY += 12.0f;

		for ( a = 0; a < MOBA_ABILITIES_PER_HERO; a++ )
		{
			const mobaAbility_t *ab = &hero->abilities[a];

			// the icon says what kind of ability it is and the letter next to it
			// is the key it goes on, so the kit is readable before the player
			// ever enters the fight
			CG_Moba_AbilityIcon( x, textY - 1.0f, 12.0f, ab, 1.0f );

			CG_Text_Paint( x + 15.0f, textY, 0.6f, colorGold, cgMobaAbilityKeys[a],
				0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
			CG_Text_Paint( x + 24.0f, textY, 0.6f, colorText, ab->name, 0, 0,
				ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
			textY += 11.0f;

			CG_Text_Paint( x, textY, 0.55f, colorDim,
				va( "%s  cd %i s", CG_Moba_AbilityTag( ab ), ab->cooldownMs / 1000 ),
				0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

			CG_Text_Paint( x + 92.0f, textY, 0.55f, colorGreen,
				va( "mana %i", ab->manaCost ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
			textY += 10.0f;

			CG_Text_Paint( x, textY, 0.55f, colorText, CG_Moba_AbilityEffect( ab ), 0, 0,
				ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
			textY += 10.0f;

			CG_Text_Paint( x, textY, 0.55f, colorHint, ab->desc, 0, 0,
				ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
			textY += 13.0f;
		}
	}
}

//=========================================================================
// Server command handler: "mobaShop phase seconds gold mask level item0 count0
// cd0 item1 count1 cd1". A new phase closes the shop, so the window of the last
// round never stands in the way of the next one.
//=========================================================================
void CG_Moba_ServerCommand_f( void )
{
	char buf[128], *p;
	int v[11], i;

	if ( !CG_Argv( 1 ) || !CG_Argv( 1 )[0] || cg_moba.integer == 0 )
	{
		return;
	}

	Q_strncpyz( buf, CG_Argv( 1 ), sizeof( buf ) );
	p = buf;
	for ( i = 0; i < 11; i++ )
	{
		v[i] = strtol( p, &p, 10 );
		while ( *p == ' ' )
		{
			p++;
		}
	}

	if ( !cgMoba.received || v[0] != cgMoba.phase )
	{
		cgMoba.open = qfalse;	// a new phase starts with the shop closed
	}

	cgMoba.phase = v[0];
	cgMoba.secondsLeft = v[1];
	cgMoba.receivedAt = cg.time;
	cgMoba.received = qtrue;
	cgMoba.gold = v[2];
	cgMoba.itemMask = v[3];
	cgMoba.level = v[4];
	cgMoba.slotItem[0] = v[5];
	cgMoba.slotCount[0] = v[6];
	cgMoba.slotCd[0] = v[7];
	cgMoba.slotItem[1] = v[8];
	cgMoba.slotCount[1] = v[9];
	cgMoba.slotCd[1] = v[10];

	if ( !cgMoba.logged )
	{
		cgMoba.logged = qtrue;

		// A build stamp on purpose: the client prints this once per session, so
		// "is this the dll I just built" is one glance in the console instead of
		// counting shop tiles. The counts are the ones this file was built with,
		// so a stale dll gives itself away here.
		trap->Print( va( "MOBA: build 05.10 items %i abilities %i item slots %i\n",
			CG_MOBA_NUM_ITEMS, CG_MOBA_ABILITIES, MOBA_ACTIVE_SLOTS ) );
		trap->Print( va( "MOBA: shop state: %s (cg_moba %i)\n", buf, cg_moba.integer ) );
	}
}

//=========================================================================
// Ability bar
//
// The two abilities are on Q and E and the two item slots on C and V. The
// letters live in moba.cfg as binds that write a token into cg_mobaAbility,
// because the JKA client turns
// keys into buttons before the cgame ever sees them (see CG_Moba_HandleToken).
// moba.cfg is where a player rebinds them, and the bar shows the same letters,
// so the picture on the screen and the config never disagree.
//
// The icons are drawn out of plain rectangles. JKA ships no art for a moba
// kit, and the stock force icons are square jpegs without an alpha channel,
// which would come out as black boxes on a dark panel.
//=========================================================================
static const char *cgMobaAbilityKeys[CG_MOBA_ABILITIES] = { "Q", "E" };

// left edge of a slot, the bar is centred under the crosshair
static float CG_Moba_BarX( int slot )
{
	float total = (float)CG_MOBA_ABILITIES * CG_MOBA_BAR_SIZE +
		( CG_MOBA_ABILITIES - 1 ) * CG_MOBA_BAR_GAP;

	return ( 640.0f - total ) * 0.5f + slot * ( CG_MOBA_BAR_SIZE + CG_MOBA_BAR_GAP );
}

static void CG_Moba_AbilityIcon( float x, float y, float s, const mobaAbility_t *ab, float alpha )
{
	static const vec4_t colorDirect	= { 0.95f, 0.95f, 1.00f, 1.00f };
	static const vec4_t colorDamage	= { 1.00f, 0.45f, 0.25f, 1.00f };
	static const vec4_t colorHeal	= { 0.35f, 0.95f, 0.45f, 1.00f };
	static const vec4_t colorBuff	= { 1.00f, 0.80f, 0.25f, 1.00f };
	const vec4_t *src;
	vec4_t color;
	int i;

	switch ( ab->type )
	{
	case AB_DIRECT:		src = &colorDirect;	break;
	case AB_AOE_DAMAGE:	src = &colorDamage;	break;
	case AB_AOE_HEAL:		src = &colorHeal;		break;
	default:			src = &colorBuff;		break;
	}

	for ( i = 0; i < 4; i++ )
	{
		color[i] = (*src)[i];
	}
	color[3] *= alpha;

	if ( ab->type == AB_DIRECT )
	{
		// a shaft and a triangular head, pointing right: one target
		CG_FillRect( x + 0.08f * s, y + 0.42f * s, 0.46f * s, 0.16f * s, color );

		for ( i = 0; i < 4; i++ )
		{
			CG_FillRect( x + ( 0.50f + 0.12f * i ) * s, y + ( 0.12f + 0.12f * i ) * s,
				0.16f * s, ( 0.76f - 0.24f * i ) * s, color );
		}
	}
	else if ( ab->type == AB_AOE_DAMAGE )
	{
		// an X: a burst that hits everything around the caster
		for ( i = 0; i < 5; i++ )
		{
			CG_FillRect( x + ( 0.14f + 0.14f * i ) * s, y + ( 0.14f + 0.14f * i ) * s,
				0.18f * s, 0.18f * s, color );
			CG_FillRect( x + ( 0.68f - 0.14f * i ) * s, y + ( 0.14f + 0.14f * i ) * s,
				0.18f * s, 0.18f * s, color );
		}
	}
	else if ( ab->type == AB_AOE_HEAL )
	{
		// a plus
		CG_FillRect( x + 0.40f * s, y + 0.14f * s, 0.20f * s, 0.72f * s, color );
		CG_FillRect( x + 0.14f * s, y + 0.40f * s, 0.72f * s, 0.20f * s, color );
	}
	else
	{
		// an arrow pointing up: a buff on the caster himself
		for ( i = 0; i < 4; i++ )
		{
			CG_FillRect( x + ( 0.16f + 0.16f * i ) * s, y + ( 0.12f + 0.14f * i ) * s,
				( 0.68f - 0.32f * i ) * s, 0.16f * s, color );
		}

		CG_FillRect( x + 0.44f * s, y + 0.60f * s, 0.12f * s, 0.28f * s, color );
	}
}

// which slot the mouse is over, -1 when it is nowhere near the bar
static int CG_Moba_BarAt( float mx, float my )
{
	int i;

	for ( i = 0; i < CG_MOBA_ABILITIES; i++ )
	{
		float x = CG_Moba_BarX( i );

		if ( mx >= x && mx < x + CG_MOBA_BAR_SIZE &&
			my >= CG_MOBA_BAR_Y && my < CG_MOBA_BAR_Y + CG_MOBA_BAR_SIZE )
		{
			return i;
		}
	}

	return -1;
}

// The bar belongs to the two phases where a hero can act. During the draft the
// hero window owns the screen and the player has no hero yet, and in the lobby
// there is nothing to cast.
static qboolean CG_Moba_BarWanted( void )
{
	if ( !cg_moba.integer || !cgMobaAb.received || cgMobaAb.heroId < 0 ||
		cgMobaAb.heroId >= MOBA_MAX_HEROES || !cgMoba.received )
	{
		return qfalse;
	}

	if ( cgMoba.phase != CG_MOBA_PHASE_BUY && cgMoba.phase != CG_MOBA_PHASE_FIGHT )
	{
		return qfalse;
	}

	// the shop panel sits on the left, the bar is in the middle: they do not
	// overlap, so both may be on screen at the same time
	return qtrue;
}

//=========================================================================
// Casting. The server owns every rule (phase, hero, life, cooldown) and answers
// with the reason when it refuses, so the client does not second guess it and
// only makes sure the same key cannot fire twice in one frame.
//=========================================================================
static void CG_Moba_CastAbility( int slot )
{
	if ( slot < 0 || slot >= CG_MOBA_ABILITIES || !CG_Moba_BarWanted() )
	{
		return;
	}

	// the same chat command a player would type, which is what the server
	// already answers: the newline is part of the command (CG_Moba_HandleToken)
	trap->SendConsoleCommand( va( "cmd say !%i\n", slot + 1 ) );
}

static void CG_Moba_AbilityInput( void )
{
	static char lastToken[16] = "0";
	char token[16];

	trap->Cvar_VariableStringBuffer( "cg_mobaAbility", token, sizeof( token ) );

	if ( Q_stricmp( token, lastToken ) == 0 )
	{
		return;
	}

	Q_strncpyz( lastToken, token, sizeof( lastToken ) );

	if ( token[0] >= '1' && token[0] <= '2' && !token[1] )
	{
		CG_Moba_CastAbility( token[0] - '1' );
	}

	// clear the token again so the same key can fire the next one
	trap->SendConsoleCommand( "set cg_mobaAbility 0\n" );
}

//=========================================================================
// Item slots
//
// The two items a player owns sit in the C and V slots, left of the ability
// bar, so both fit on one row under the crosshair. The server pushes the item
// id, the charge count and the remaining cooldown of every slot; the client only
// draws them and turns a key press into !use1 / !use2, which is the chat command
// a player could have typed. The server owns whether a use happens at all.
//
// The icons are drawn out of plain rectangles. JKA ships no art for a moba kit
// and the stock force icons are square jpegs without an alpha channel, which
// would come out as black boxes on a dark panel.
//=========================================================================
static const char *cgMobaItemKeys[2] = { "C", "V" };

// what is left of a slot cooldown right now, the push only samples it
static int CG_Moba_ItemCooldownLeft( int slot )
{
	int left;

	if ( !cgMoba.received || slot < 0 || slot > 1 )
	{
		return 0;
	}

	left = cgMoba.slotCd[slot] - ( cg.time - cgMoba.receivedAt );

	return ( left > 0 ) ? left : 0;
}

// The glyph painter. Every item is drawn from a handful of rectangles inside a
// square of size s at (x,y), which is what the tile in the shop window and the
// slot on the bar both call.
static void CG_Moba_ItemIcon( float x, float y, float s, int item, float alpha )
{
	static const vec4_t colorSteel		= { 0.62f, 0.72f, 0.85f, 1.00f };
	static const vec4_t colorWhite		= { 0.95f, 0.95f, 0.95f, 1.00f };
	static const vec4_t colorRed		= { 0.90f, 0.28f, 0.22f, 1.00f };
	static const vec4_t colorOrange	= { 0.95f, 0.55f, 0.15f, 1.00f };
	static const vec4_t colorPurple	= { 0.70f, 0.40f, 0.90f, 1.00f };
	static const vec4_t colorGreen		= { 0.35f, 0.80f, 0.50f, 1.00f };
	static const vec4_t colorBlue		= { 0.35f, 0.70f, 0.95f, 1.00f };
	static const vec4_t colorDark		= { 0.25f, 0.28f, 0.38f, 1.00f };
	const vec4_t *c;
	vec4_t color;
	int i;

	switch ( item )
	{
	case MOBA_ITEM_STURDY_ARMOR:	c = &colorSteel;	break;
	case MOBA_ITEM_MEDKIT:			c = &colorRed;	break;
	case MOBA_ITEM_RAGE_RUNE:		c = &colorOrange;	break;
	case MOBA_ITEM_HEAVY_PLATE:	c = &colorSteel;	break;
	case MOBA_ITEM_POWER_CRYSTAL:	c = &colorPurple;	break;
	case MOBA_ITEM_SHADOW_CLOAK:	c = &colorGreen;	break;
	case MOBA_ITEM_GRENADE:		c = &colorDark;	break;
	case MOBA_ITEM_UMBRELLA:		c = &colorBlue;	break;
	case MOBA_ITEM_INVIS_CLOAK:	c = &colorWhite;	break;
	default:						c = &colorWhite;	break;
	}

	for ( i = 0; i < 4; i++ )
	{
		color[i] = (*c)[i];
	}
	color[3] = alpha;

	switch ( item )
	{
	case MOBA_ITEM_STURDY_ARMOR:
	case MOBA_ITEM_HEAVY_PLATE:
		// a shield: a rounded body with a boss in the middle
		CG_FillRect( x + 0.18f * s, y + 0.10f * s, 0.64f * s, 0.36f * s, color );
		CG_FillRect( x + 0.10f * s, y + 0.20f * s, 0.16f * s, 0.20f * s, color );
		CG_FillRect( x + 0.74f * s, y + 0.20f * s, 0.16f * s, 0.20f * s, color );
		CG_FillRect( x + 0.26f * s, y + 0.46f * s, 0.48f * s, 0.10f * s, color );
		CG_FillRect( x + 0.34f * s, y + 0.56f * s, 0.32f * s, 0.10f * s, color );
		CG_FillRect( x + 0.42f * s, y + 0.66f * s, 0.16f * s, 0.10f * s, color );
		CG_FillRect( x + 0.42f * s, y + 0.20f * s, 0.16f * s, 0.16f * s, color );
		break;

	case MOBA_ITEM_MEDKIT:
		// a white case with a cross, the one image everybody knows
		CG_FillRect( x + 0.12f * s, y + 0.26f * s, 0.76f * s, 0.56f * s, color );
		CG_FillRect( x + 0.24f * s, y + 0.14f * s, 0.52f * s, 0.14f * s, color );
		CG_FillRect( x + 0.42f * s, y + 0.36f * s, 0.16f * s, 0.36f * s, colorWhite );
		CG_FillRect( x + 0.32f * s, y + 0.46f * s, 0.36f * s, 0.16f * s, colorWhite );
		break;

	case MOBA_ITEM_RAGE_RUNE:
		// a rune: a diamond with a bar through it
		for ( i = 0; i < 4; i++ )
		{
			CG_FillRect( x + ( 0.18f + 0.16f * i ) * s, y + ( 0.14f + 0.16f * i ) * s,
				( 0.68f - 0.32f * i ) * s, ( 0.68f - 0.32f * i ) * s, color );
		}
		CG_FillRect( x + 0.06f * s, y + 0.44f * s, 0.88f * s, 0.12f * s, colorWhite );
		break;

	case MOBA_ITEM_POWER_CRYSTAL:
		// a crystal: a tall diamond, the mana pool next to a fire rune
		CG_FillRect( x + 0.34f * s, y + 0.06f * s, 0.32f * s, 0.20f * s, color );
		CG_FillRect( x + 0.22f * s, y + 0.24f * s, 0.56f * s, 0.22f * s, color );
		CG_FillRect( x + 0.10f * s, y + 0.44f * s, 0.80f * s, 0.22f * s, color );
		CG_FillRect( x + 0.22f * s, y + 0.64f * s, 0.56f * s, 0.20f * s, color );
		CG_FillRect( x + 0.34f * s, y + 0.82f * s, 0.32f * s, 0.12f * s, color );
		CG_FillRect( x + 0.44f * s, y + 0.30f * s, 0.10f * s, 0.40f * s, colorWhite );
		break;

	case MOBA_ITEM_SHADOW_CLOAK:
		// a cloak: a hood over shoulders that narrow to the feet
		CG_FillRect( x + 0.26f * s, y + 0.08f * s, 0.48f * s, 0.16f * s, color );
		CG_FillRect( x + 0.34f * s, y + 0.24f * s, 0.32f * s, 0.14f * s, color );
		CG_FillRect( x + 0.18f * s, y + 0.38f * s, 0.64f * s, 0.20f * s, color );
		CG_FillRect( x + 0.26f * s, y + 0.58f * s, 0.48f * s, 0.16f * s, color );
		CG_FillRect( x + 0.34f * s, y + 0.74f * s, 0.32f * s, 0.16f * s, color );
		break;

	case MOBA_ITEM_GRENADE:
		// a thermal detonator: a body, a neck and a plunger on top
		CG_FillRect( x + 0.22f * s, y + 0.26f * s, 0.56f * s, 0.62f * s, color );
		CG_FillRect( x + 0.38f * s, y + 0.12f * s, 0.24f * s, 0.16f * s, colorSteel );
		CG_FillRect( x + 0.28f * s, y + 0.02f * s, 0.44f * s, 0.12f * s, colorRed );
		CG_FillRect( x + 0.32f * s, y + 0.44f * s, 0.36f * s, 0.10f * s, colorOrange );
		CG_FillRect( x + 0.32f * s, y + 0.62f * s, 0.36f * s, 0.10f * s, colorOrange );
		break;

	case MOBA_ITEM_UMBRELLA:
		// an umbrella: a dome over a shaft, the shape everybody knows
		CG_FillRect( x + 0.16f * s, y + 0.34f * s, 0.68f * s, 0.12f * s, color );
		CG_FillRect( x + 0.10f * s, y + 0.22f * s, 0.80f * s, 0.14f * s, color );
		CG_FillRect( x + 0.24f * s, y + 0.10f * s, 0.52f * s, 0.14f * s, color );
		CG_FillRect( x + 0.44f * s, y + 0.02f * s, 0.12f * s, 0.10f * s, color );
		CG_FillRect( x + 0.46f * s, y + 0.46f * s, 0.08f * s, 0.46f * s, colorWhite );
		CG_FillRect( x + 0.38f * s, y + 0.88f * s, 0.24f * s, 0.08f * s, colorWhite );
		break;

	case MOBA_ITEM_INVIS_CLOAK:
		// a cloaked silhouette that fades out upwards, plus the empty outline
		// it starts from and ends in
		for ( i = 0; i < 5; i++ )
		{
			CG_FillRect( x + ( 0.22f + 0.06f * i ) * s, y + ( 0.56f - 0.10f * i ) * s,
				( 0.56f - 0.12f * i ) * s, ( 0.12f + 0.04f * i ) * s, color );
		}
		CG_FillRect( x + 0.34f * s, y + 0.06f * s, 0.32f * s, 0.26f * s, color );
		CG_FillRect( x + 0.06f * s, y + 0.16f * s, 0.88f * s, 0.04f * s, color );
		break;

	default:
		CG_FillRect( x + 0.20f * s, y + 0.20f * s, 0.60f * s, 0.60f * s, color );
		break;
	}
}

// left edge of an item slot, the two slots sit left of the ability bar
static float CG_Moba_SlotX( int slot )
{
	return CG_Moba_BarX( 0 ) - ( 2 - slot ) * ( CG_MOBA_BAR_SIZE + CG_MOBA_BAR_GAP );
}

static qboolean CG_Moba_SlotsWanted( void )
{
	if ( !cg_moba.integer || !cgMoba.received )
	{
		return qfalse;
	}

	if ( cgMoba.phase != CG_MOBA_PHASE_BUY && cgMoba.phase != CG_MOBA_PHASE_FIGHT )
	{
		return qfalse;
	}

	return qtrue;
}

static void CG_Moba_UseSlot( int slot )
{
	if ( slot < 0 || slot > 1 || !CG_Moba_SlotsWanted() )
	{
		return;
	}

	trap->SendConsoleCommand( va( "cmd say !use%i\n", slot + 1 ) );
}

// The binds in moba.cfg write the slot number into cg_mobaItem, one token per
// press, and the cgame reads and clears that cvar once a frame. It is a cvar of
// its own and not cg_mobaAbility, because a letter can be an ability or an item
// and both tokens have to be able to sit next to each other for one frame.
static void CG_Moba_ItemInput( void )
{
	static char lastToken[16] = "0";
	char token[16];

	trap->Cvar_VariableStringBuffer( "cg_mobaItem", token, sizeof( token ) );

	if ( Q_stricmp( token, lastToken ) == 0 )
	{
		return;
	}

	Q_strncpyz( lastToken, token, sizeof( lastToken ) );

	if ( token[0] >= '1' && token[0] <= '2' && !token[1] )
	{
		CG_Moba_UseSlot( token[0] - '1' );
	}

	trap->SendConsoleCommand( "set cg_mobaItem 0\n" );
}

// The two item slots. Empty slots are drawn as a dark box with the key in it,
// because a player who bought nothing still has to see where the items would
// go.
static void CG_Moba_DrawItemSlots( void )
{
	static vec4_t colorBg		= { 0.02f, 0.02f, 0.05f, 0.80f };
	static vec4_t colorBorder	= { 0.60f, 0.50f, 0.20f, 0.95f };
	static vec4_t colorReady	= { 0.30f, 0.90f, 0.40f, 0.90f };
	static vec4_t colorWait		= { 0.35f, 0.35f, 0.40f, 0.90f };
	static vec4_t colorEmpty	= { 0.25f, 0.25f, 0.30f, 0.70f };
	static vec4_t colorShade	= { 0.00f, 0.00f, 0.00f, 0.65f };
	static vec4_t colorKey		= { 1.00f, 0.85f, 0.30f, 1.00f };
	static vec4_t colorText		= { 1.00f, 1.00f, 1.00f, 1.00f };
	static vec4_t colorDim		= { 0.60f, 0.60f, 0.60f, 1.00f };

	float x, w, frac;
	int i, id, left, count, total;

	if ( !CG_Moba_SlotsWanted() )
	{
		return;
	}

	// the name of a slot only while the mouse is on it, same rule as the bar
	for ( i = 0; i < 2; i++ )
	{
		x = CG_Moba_SlotX( i );
		id = cgMoba.slotItem[i];
		left = CG_Moba_ItemCooldownLeft( i );
		count = cgMoba.slotCount[i];

		if ( id >= 0 && id < CG_MOBA_NUM_ITEMS )
		{
			const char *tip = cgMobaItems[id].name;

			if ( cgMobaItems[id].maxCount > 1 )
			{
				tip = va( "%s  x%i", tip, count );
			}

			if ( left > 0 )
			{
				tip = va( "%s  -  %i s", tip, ( left + 999 ) / 1000 );
			}

			if ( cg_moba.integer && cgs.cursorX >= (int)x &&
				cgs.cursorX < (int)( x + CG_MOBA_BAR_SIZE ) &&
				cgs.cursorY >= (int)CG_MOBA_BAR_Y &&
				cgs.cursorY < (int)( CG_MOBA_BAR_Y + CG_MOBA_BAR_SIZE ) )
			{
				w = (float)CG_Text_Width( tip, 0.6f, FONT_SMALL );
				CG_Text_Paint( ( 640.0f - w ) * 0.5f, CG_MOBA_BAR_Y - 12.0f, 0.6f,
					( left > 0 ) ? colorDim : colorText, tip, 0, 0,
					ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
			}
		}

		CG_FillRect( x, CG_MOBA_BAR_Y, CG_MOBA_BAR_SIZE, CG_MOBA_BAR_SIZE, colorBg );

		if ( id >= 0 && id < CG_MOBA_NUM_ITEMS )
		{
			CG_DrawRect( x, CG_MOBA_BAR_Y, CG_MOBA_BAR_SIZE, CG_MOBA_BAR_SIZE, 1.0f,
				( left > 0 ) ? colorWait : colorReady );

			CG_Moba_ItemIcon( x + 5.0f, CG_MOBA_BAR_Y + 5.0f, 12.0f, id,
				( left > 0 ) ? 0.35f : 1.0f );

			// a stacking item shows how many charges are left, because a grenade
			// at x1 looks the same as one that is about to be gone
			if ( cgMobaItems[id].maxCount > 1 )
			{
				CG_Text_Paint( x + CG_MOBA_BAR_SIZE - 7.0f, CG_MOBA_BAR_Y, 0.45f,
					colorText, va( "%i", count ), 0, 0,
					ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
			}

			if ( left > 0 )
			{
				// the wipe comes down from the top over the whole slot, the same
				// way the ability bar does it
				total = cgMobaItems[id].cooldownMs;
				frac = ( total > 0 ) ? (float)left / (float)total : 0.0f;

				if ( frac > 1.0f )
				{
					frac = 1.0f;
				}

				CG_FillRect( x + 1.0f, CG_MOBA_BAR_Y + 1.0f, CG_MOBA_BAR_SIZE - 2.0f,
					( CG_MOBA_BAR_SIZE - 2.0f ) * frac, colorShade );

				w = (float)CG_Text_Width( va( "%i", ( left + 999 ) / 1000 ), 0.55f, FONT_SMALL );
				CG_Text_Paint( x + ( CG_MOBA_BAR_SIZE - w ) * 0.5f, CG_MOBA_BAR_Y + 6.0f,
					0.55f, colorText, va( "%i", ( left + 999 ) / 1000 ), 0, 0,
					ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
			}
		}
		else
		{
			// empty slot: a dashed box, no icon, so it never looks like an item
			CG_DrawRect( x, CG_MOBA_BAR_Y, CG_MOBA_BAR_SIZE, CG_MOBA_BAR_SIZE, 1.0f,
				colorEmpty );
			CG_FillRect( x + 6.0f, CG_MOBA_BAR_Y + 10.0f, 10.0f, 2.0f, colorEmpty );
		}

		CG_Text_Paint( x + 1.0f, CG_MOBA_BAR_Y, 0.42f, colorKey,
			cgMobaItemKeys[i], 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}
}

//=========================================================================
// Flame channel input. The server pays the flame per second, so the client
// only has to repeat the cast while the key is held; every repeat pushes the
// channel a little further and the server keeps the channel alive as long as
// the repeats keep coming. A missing repeat (a stall, a menu) is covered by the
// server's grace time, a release stops it at once.
//=========================================================================
static qboolean cgMobaFlameHeld = qfalse;
static int cgMobaFlameNext = 0;

// the slot of the hero's flame ability, -1 when the hero has none
static int CG_Moba_FlameSlot( void )
{
	const mobaHero_t *hero;
	int i;

	if ( !cgMobaAb.received || cgMobaAb.heroId < 0 || cgMobaAb.heroId >= MOBA_MAX_HEROES )
	{
		return -1;
	}

	hero = &mobaHeroTable[cgMobaAb.heroId];

	for ( i = 0; i < CG_MOBA_ABILITIES; i++ )
	{
		if ( hero->abilities[i].type == AB_FLAME )
		{
			return i;
		}
	}

	return -1;
}

static void CG_Moba_FlameInput( void )
{
	int slot;

	if ( !cgMobaFlameHeld || !CG_Moba_BarWanted() || cg.time < cgMobaFlameNext )
	{
		return;
	}

	slot = CG_Moba_FlameSlot();

	if ( slot < 0 )
	{
		return;
	}

	cgMobaFlameNext = cg.time + 200;
	CG_Moba_CastAbility( slot );
}

void CG_Moba_FlameDown_f( void )
{
	int slot = CG_Moba_FlameSlot();

	cgMobaFlameHeld = qtrue;
	cgMobaFlameNext = 0;

	// the key is the second ability slot: with a flame it channels, without one
	// it is a plain instant cast, fired once here so the press is never lost
	if ( slot != 1 && CG_Moba_BarWanted() )
	{
		CG_Moba_CastAbility( 1 );
	}
}

void CG_Moba_FlameUp_f( void )
{
	cgMobaFlameHeld = qfalse;
}

static void CG_Moba_DrawAbilityBar( void )
{
	static vec4_t colorBg		= { 0.02f, 0.02f, 0.05f, 0.80f };
	static vec4_t colorBorder	= { 0.60f, 0.50f, 0.20f, 0.95f };
	static vec4_t colorReady	= { 0.30f, 0.90f, 0.40f, 0.90f };
	static vec4_t colorWait		= { 0.35f, 0.35f, 0.40f, 0.90f };
	static vec4_t colorShade	= { 0.00f, 0.00f, 0.00f, 0.65f };
	static vec4_t colorKey		= { 1.00f, 0.85f, 0.30f, 1.00f };
	static vec4_t colorText		= { 1.00f, 1.00f, 1.00f, 1.00f };
	static vec4_t colorName		= { 0.80f, 0.80f, 0.80f, 1.00f };
	static vec4_t colorDim		= { 0.60f, 0.60f, 0.60f, 1.00f };

	const mobaHero_t *hero = &mobaHeroTable[cgMobaAb.heroId];
	const mobaAbility_t *ab;
	float x, w;
	int i, left, rank, hover;

	if ( !CG_Moba_BarWanted() )
	{
		return;
	}

	// the name of one slot only: four names under four small boxes would run
	// into each other and hide the fight behind them
	hover = CG_Moba_BarAt( (float)cgs.cursorX, (float)cgs.cursorY );

	if ( hover >= 0 )
	{
		const char *tip;

		ab = &hero->abilities[hover];
		left = CG_Moba_CooldownLeft( hover );
		tip = ( left > 0 ) ? va( "%s  -  %i s", ab->name, ( left + 999 ) / 1000 ) : ab->name;

		w = (float)CG_Text_Width( tip, 0.6f, FONT_SMALL );
		CG_Text_Paint( ( 640.0f - w ) * 0.5f, CG_MOBA_BAR_Y - 12.0f, 0.6f,
			( left > 0 ) ? colorDim : colorName, tip, 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}

	for ( i = 0; i < CG_MOBA_ABILITIES; i++ )
	{
		ab = &hero->abilities[i];
		left = CG_Moba_CooldownLeft( i );
		rank = cgMobaAb.level[i];
		x = CG_Moba_BarX( i );

		CG_FillRect( x, CG_MOBA_BAR_Y, CG_MOBA_BAR_SIZE, CG_MOBA_BAR_SIZE, colorBg );
		CG_DrawRect( x, CG_MOBA_BAR_Y, CG_MOBA_BAR_SIZE, CG_MOBA_BAR_SIZE, 1.0f,
			( left > 0 ) ? colorWait : colorReady );

		CG_Moba_AbilityIcon( x + 5.0f, CG_MOBA_BAR_Y + 5.0f, 12.0f, ab,
			( left > 0 ) ? 0.35f : 1.0f );

		// the key in the corner and the rank next to it, so the player can see at
		// a glance what is on which key and what he upgraded
		CG_Text_Paint( x + 1.0f, CG_MOBA_BAR_Y, 0.42f, colorKey,
			cgMobaAbilityKeys[i], 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		if ( rank > 0 )
		{
			CG_Text_Paint( x + CG_MOBA_BAR_SIZE - 7.0f, CG_MOBA_BAR_Y, 0.42f,
				colorText, va( "%i", rank ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}

		if ( left > 0 )
		{
			// the part of the icon that is still on cooldown, counted from the
			// top, and the seconds left on top of it
			float frac = ( (float)ab->cooldownMs > 0.0f ) ?
				(float)left / (float)ab->cooldownMs : 0.0f;

			if ( frac > 1.0f )
			{
				frac = 1.0f;
			}

			CG_FillRect( x + 1.0f, CG_MOBA_BAR_Y + 1.0f, CG_MOBA_BAR_SIZE - 2.0f,
				( CG_MOBA_BAR_SIZE - 2.0f ) * frac, colorShade );

			w = (float)CG_Text_Width( va( "%i", ( left + 999 ) / 1000 ), 0.55f, FONT_SMALL );
			CG_Text_Paint( x + ( CG_MOBA_BAR_SIZE - w ) * 0.5f, CG_MOBA_BAR_Y + 6.0f,
				0.55f, colorText, va( "%i", ( left + 999 ) / 1000 ), 0, 0,
				ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}
	}

	// a running effect gets its own row of icons five pixels above the bar, one
	// per slot that has something going, drawn exactly like an ability icon
	{
		static vec4_t colorEffectBg	= { 0.02f, 0.05f, 0.10f, 0.70f };
		static vec4_t colorEffect	= { 0.45f, 0.75f, 1.00f, 0.95f };
		float ey = CG_MOBA_BAR_Y - CG_MOBA_BAR_SIZE - 5.0f;

		for ( i = 0; i < CG_MOBA_ABILITIES; i++ )
		{
			int eff = CG_Moba_EffectLeft( i );

			if ( eff <= 0 )
			{
				continue;
			}

			x = CG_Moba_BarX( i );
			ab = &hero->abilities[i];

			CG_FillRect( x, ey, CG_MOBA_BAR_SIZE, CG_MOBA_BAR_SIZE, colorEffectBg );
			CG_DrawRect( x, ey, CG_MOBA_BAR_SIZE, CG_MOBA_BAR_SIZE, 1.0f, colorEffect );

			CG_Moba_AbilityIcon( x + 5.0f, ey + 5.0f, 12.0f, ab, 1.0f );

			// the seconds left of the effect in the corner, same as the cooldown
			w = (float)CG_Text_Width( va( "%i", ( eff + 999 ) / 1000 ), 0.42f, FONT_SMALL );
			CG_Text_Paint( x + CG_MOBA_BAR_SIZE - w - 1.0f, ey, 0.42f, colorText,
				va( "%i", ( eff + 999 ) / 1000 ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}
	}
}

//=========================================================================
// Server command handler for the ability bar:
// "mobaAbilities heroId cd0 cd1 cd2 cd3 lv0 lv1 lv2 lv3 mana maxMana
//  eff0 eff1 eff2 eff3".
//
// The hero id comes first so the bar knows whose four abilities it draws, and
// the cooldowns are milliseconds rather than seconds: a 6 second cooldown that
// ticks in whole seconds would sit at "1" for a full second and then jump. The
// mana pair lets the counter be drawn without server traffic of its own, and the
// last four numbers are the effect times that drive the buff icons above the bar.
//=========================================================================
void CG_Moba_AbilitiesCommand_f( void )
{
	char buf[96], *p;
	int v[15], i;

	if ( !CG_Argv( 1 ) || !CG_Argv( 1 )[0] || cg_moba.integer == 0 )
	{
		return;
	}

	Q_strncpyz( buf, CG_Argv( 1 ), sizeof( buf ) );
	p = buf;
	for ( i = 0; i < 15; i++ )
	{
		v[i] = strtol( p, &p, 10 );
		while ( *p == ' ' )
		{
			p++;
		}
	}

	cgMobaAb.heroId = v[0];

	for ( i = 0; i < CG_MOBA_ABILITIES; i++ )
	{
		cgMobaAb.cooldown[i] = v[1 + i];
		cgMobaAb.level[i] = v[5 + i];
		cgMobaAb.effect[i] = v[11 + i];
	}

	cgMobaAb.mana = v[9];
	cgMobaAb.maxMana = v[10];

	cgMobaAb.receivedAt = cg.time;
	cgMobaAb.received = qtrue;

	// Only the headless test reads this, and it has to stay rare: once per hero,
	// and once for the first cooldown that is actually running, which is what
	// proves the bar got a timer to draw.
	if ( cgMobaAb.loggedHero != cgMobaAb.heroId || !cgMobaAb.loggedCooldown )
	{
		qboolean running = qfalse;

		for ( i = 0; i < CG_MOBA_ABILITIES; i++ )
		{
			if ( cgMobaAb.cooldown[i] > 0 )
			{
				running = qtrue;
			}
		}

		if ( running )
		{
			cgMobaAb.loggedCooldown = qtrue;
		}

		if ( cgMobaAb.loggedHero != cgMobaAb.heroId || running )
		{
			cgMobaAb.loggedHero = cgMobaAb.heroId;
			trap->Print( va( "MOBA: ability state received: %s\n", buf ) );
		}
	}
}

//=========================================================================
// Mana counter. The vanilla force bar (bottom right) keeps working as the
// fatigue bar, spent on jumps and combo attacks. The mana pool is shown as a
// number next to the health and armor counters of the lefthud menu, in the
// same mana green as the old crystals. The server pushes the current value five
// times a second and the client fills the pool up with the regen rate in
// between, exactly like a cooldown.
//
// The lefthud menu owns the health/armor counters, their rects are read out of
// that menu and the mana counter is placed to the right of the armor counter.
// When hud.menu positions differ the mana counter simply follows along.
//=========================================================================
void CG_Moba_InvisCommand_f( void )
{
	int clientNum, left, mode;

	if ( !CG_Argv( 1 ) || !CG_Argv( 1 )[0] || cg_moba.integer == 0 )
	{
		return;
	}

	clientNum = atoi( CG_Argv( 1 ) );
	left = atoi( CG_Argv( 2 ) );
	mode = atoi( CG_Argv( 3 ) );

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS )
	{
		return;
	}

	// An old server sends no mode at all. Treating that as "invisible" keeps the
	// cloak working against a mixed build instead of leaving the body on screen
	// for everybody.
	if ( !CG_Argv( 3 )[0] )
	{
		mode = MOBA_INVIS_HIDDEN;
	}

	if ( mode < MOBA_INVIS_NONE || mode > MOBA_INVIS_GHOST )
	{
		mode = MOBA_INVIS_NONE;
	}

	// The server already drops a repeat that says the same thing, so whatever
	// arrives here is a change: a new cloak, a shorter one, a viewer that
	// changed sides, or the end of it. cg_players.c counts the time down from
	// here, so the fade runs on the client and not as a stream of per frame
	// broadcasts.
	if ( cg.mobaInvisLeft[clientNum] != left || cg.mobaInvisMode[clientNum] != mode )
	{
		cg.mobaInvisLeft[clientNum] = left;
		cg.mobaInvisTime[clientNum] = cg.time;
		cg.mobaInvisMode[clientNum] = mode;
	}
}

void CG_Moba_DrawMana( menuDef_t *menuHUD )
{
	static const vec4_t colorMana	= { 0.25f, 0.55f, 1.00f, 1.00f };
	float mana;
	int manaInt;
	int x, y, w, h;
	itemDef_t *focusItem;

	if ( cg_moba.integer == 0 || !cgMobaAb.received || cgMobaAb.heroId < 0 ||
		!menuHUD )
	{
		return;
	}

	if ( cgMoba.phase != CG_MOBA_PHASE_BUY && cgMoba.phase != CG_MOBA_PHASE_FIGHT )
	{
		// the counter belongs to the buy and the fight phase, the same ones that
		// show the ability bar
		return;
	}

	if ( cgMobaAb.maxMana <= 0 )
	{
		return;
	}

	// mana regens between pushes: the server sent a snapshot, add the fraction
	// of the regen rate that elapsed since it was taken
	mana = (float)( cgMobaAb.mana +
		( cg.time - cgMobaAb.receivedAt ) * MOBA_MANA_REGEN_PER_SEC / 1000 );

	if ( mana > cgMobaAb.maxMana )
	{
		mana = (float)cgMobaAb.maxMana;
	}

	if ( mana < 0 )
	{
		mana = 0;
	}

	manaInt = (int)mana;

	// anchor to the armor counter so the number lands right next to it
	focusItem = Menu_FindItemByName( menuHUD, "armoramount" );
	if ( !focusItem )
	{
		return;
	}

	x = (int)focusItem->window.rect.x + (int)focusItem->window.rect.w * 3 + 7;
	y = (int)focusItem->window.rect.y;
	w = (int)focusItem->window.rect.w;
	h = (int)focusItem->window.rect.h;

	trap->R_SetColor( colorMana );

	CG_DrawNumField( x, y, 3, manaInt, w, h, NUM_FONT_SMALL, qfalse );

	trap->R_SetColor( NULL );
}
//=========================================================================
// The shop is only usable during the buy phase, because the server refuses
// purchases everywhere else (g_moba.c MOBA_BuyItem). The pm_type is
// deliberately not checked: the mod drives its own phase flow and may keep the
// player in a spectator or freeze state (the local player of a listen server
// even lands in the spectator seat), while the server only pushes shop state
// to the clients that really take part in a match. So the phase plus a living
// player is the authority here, the snapshot is only asked whether the body is
// alive.
//=========================================================================
static qboolean CG_Moba_CanShop( void )
{
	if ( !cg_moba.integer || !cgMoba.received || !cg.snap ||
		cgMoba.phase != CG_MOBA_PHASE_BUY )
	{
		return qfalse;
	}

	return ( cg.snap->ps.stats[STAT_HEALTH] > 0 ) ? qtrue : qfalse;
}

static qboolean CG_Moba_ShopOpen( void )
{
	return ( CG_Moba_CanShop() && cgMoba.open ) ? qtrue : qfalse;
}

//=========================================================================
// Input. The JKA client never hands raw key codes to the cgame: keys are turned
// into buttons before they reach the vm, so the cgame's CG_KEY_EVENT is dead
// code and a cgame side key hook can never see "B". Instead the bind in
// moba.cfg writes a token into the cg_mobaKey cvar and the window reacts to the
// change once per frame:
//
//   b - open or close the shop
//
// Escape never reaches a bind: cl_keys.cpp handles it before CL_ParseBinding and
// opens the game menu instead, so the cgame never sees that key. ESC closing the
// shop is read out of the catcher bit the client took away, see CG_Moba_Catcher.
// Everything inside the window is the mouse, plus the arrow keys and Enter,
// which do arrive while the window holds KEYCATCH_CGAME.
//=========================================================================
static void CG_Moba_HandleToken( const char *token )
{
	if ( !Q_stricmp( token, "b" ) )
	{
		if ( CG_Moba_CanShop() )
		{
			cgMoba.open = !cgMoba.open;

			// a new opening starts on the first tab and forgets the keyboard
			// cursor, the shop of the last round is not where this one is
			if ( cgMoba.open )
			{
				cgMoba.category = 0;
				cgMoba.cursor = -1;
			}
		}
		else
		{
			// say why nothing happened instead of a dead key press
			cgMoba.noticeUntil = cg.time + 2500;
		}
	}
}

static void CG_Moba_Input( void )
{
	static char lastToken[16] = "0";
	char token[16];

	trap->Cvar_VariableStringBuffer( "cg_mobaKey", token, sizeof( token ) );
	if ( Q_stricmp( token, lastToken ) == 0 )
	{
		return;
	}

	Q_strncpyz( lastToken, token, sizeof( lastToken ) );
	CG_Moba_HandleToken( token );

	// clear the token again so the same key can fire the next one
	trap->SendConsoleCommand( "set cg_mobaKey 0\n" );
}

//=========================================================================
// Shop window
//
// A window like the hero window: it takes the mouse while it is open, has
// three tabs for the three groups of items and buys with a left click. The
// server owns the price it charges and answers a refused purchase with the
// reason, so this only ever sends the !buy a player would type.
//
// The geometry is fixed, so every hit test is a rectangle compare. Prices are
// right aligned with CG_Text_Width because the JKA font is proportional and
// would break a fixed column layout.
//=========================================================================
#define CG_MOBA_SHOP_X			60.0f
#define CG_MOBA_SHOP_Y			100.0f
#define CG_MOBA_SHOP_W			520.0f
#define CG_MOBA_SHOP_H			252.0f

#define CG_MOBA_TAB_H			18.0f
#define CG_MOBA_TAB_Y			( CG_MOBA_SHOP_Y + 40.0f )
#define CG_MOBA_TAB_GAP			2.0f
#define CG_MOBA_TAB_W			( ( CG_MOBA_SHOP_W - 20.0f - 2.0f * CG_MOBA_TAB_GAP ) / 3.0f )

// three by two tiles is what six items need, and the most any group holds today
#define CG_MOBA_SHOP_COLS		3
#define CG_MOBA_SHOP_ROWS		2
#define CG_MOBA_SHOP_TILE_W		( ( CG_MOBA_SHOP_W - 20.0f - 8.0f ) / 3.0f )
#define CG_MOBA_SHOP_TILE_H		60.0f
#define CG_MOBA_SHOP_TILE_GAP	4.0f
#define CG_MOBA_SHOP_TILE_Y		( CG_MOBA_TAB_Y + CG_MOBA_TAB_H + 6.0f )
#define CG_MOBA_SHOP_SLOTS		( CG_MOBA_SHOP_COLS * CG_MOBA_SHOP_ROWS )

static float CG_Moba_TabX( int tab )
{
	return CG_MOBA_SHOP_X + 10.0f + tab * ( CG_MOBA_TAB_W + CG_MOBA_TAB_GAP );
}

static float CG_Moba_ShopTileX( int slot )
{
	return CG_MOBA_SHOP_X + 10.0f + ( slot % CG_MOBA_SHOP_COLS ) *
		( CG_MOBA_SHOP_TILE_W + CG_MOBA_SHOP_TILE_GAP );
}

static float CG_Moba_ShopTileY( int slot )
{
	return CG_MOBA_SHOP_TILE_Y + ( slot / CG_MOBA_SHOP_COLS ) *
		( CG_MOBA_SHOP_TILE_H + CG_MOBA_SHOP_TILE_GAP );
}

// the item of a grid slot, -1 when this group has fewer items than that slot
static int CG_Moba_ShopItem( int slot )
{
	int i, seen = 0;

	for ( i = 0; i < CG_MOBA_NUM_ITEMS; i++ )
	{
		if ( cgMobaItems[i].category != cgMoba.category )
		{
			continue;
		}

		if ( seen == slot )
		{
			return i;
		}

		seen++;
	}

	return -1;
}

//=========================================================================
// Hit tests
//=========================================================================
static int CG_Moba_ShopTabAt( float mx, float my )
{
	int i;

	for ( i = 0; i < CG_MOBA_ITEM_CATS; i++ )
	{
		float x = CG_Moba_TabX( i );

		if ( mx >= x && mx < x + CG_MOBA_TAB_W &&
			my >= CG_MOBA_TAB_Y && my < CG_MOBA_TAB_Y + CG_MOBA_TAB_H )
		{
			return i;
		}
	}

	return -1;
}

static int CG_Moba_ShopTileAt( float mx, float my )
{
	int slot;

	for ( slot = 0; slot < CG_MOBA_SHOP_SLOTS; slot++ )
	{
		float x = CG_Moba_ShopTileX( slot );
		float y = CG_Moba_ShopTileY( slot );

		if ( mx >= x && mx < x + CG_MOBA_SHOP_TILE_W &&
			my >= y && my < y + CG_MOBA_SHOP_TILE_H )
		{
			return CG_Moba_ShopItem( slot );
		}
	}

	return -1;
}

//=========================================================================
// Would the server take this purchase right now? Only the look of a tile
// depends on it, the click always asks the server, which owns the price and the
// slot limit. The two rules that matter: an item the player already carries can
// only be bought again while it still has charges left, and a third item cannot
// be bought at all once both slots are taken.
//=========================================================================
static qboolean CG_Moba_ShopBuyable( int item )
{
	int i;

	if ( item < 0 || item >= CG_MOBA_NUM_ITEMS )
	{
		return qfalse;
	}

	for ( i = 0; i < 2; i++ )
	{
		if ( cgMoba.slotItem[i] == item )
		{
			// already in a slot: only a stacking item can go on top of it, and
			// only while the stack is not full
			if ( cgMobaItems[item].maxCount > cgMoba.slotCount[i] )
			{
				break;
			}

			return qfalse;
		}
	}

	// not carried yet, so it needs an empty slot to land in
	for ( i = 0; i < 2; i++ )
	{
		if ( cgMoba.slotItem[i] < 0 )
		{
			break;
		}
	}

	if ( i >= 2 )
	{
		return qfalse;
	}

	return ( cgMoba.gold >= cgMobaItems[item].price ) ? qtrue : qfalse;
}

static void CG_Moba_ShopAct( int item )
{
	if ( !CG_Moba_ShopBuyable( item ) )
	{
		return;
	}

	// The vm console command queue has no separator, so the newline has to be
	// part of every command the cgame sends (cl_cgame.cpp Cbuf_AddText),
	// otherwise it merges with the next one.
	//
	// "cmd say ..." instead of a plain "say ...": the client only forwards a
	// console command to the server after the cgame, the ui and the cvar lookups
	// have all declined it, and "cmd" skips that chain completely (cl_main.cpp
	// CL_ForwardToServer_f).
	trap->SendConsoleCommand( va( "cmd say !buy %i\n", item + 1 ) );
}

// The keyboard cursor is a grid slot, so the arrow keys walk the same tiles the
// mouse walks and Enter stands in for a click. The cursor jumps back to the
// first tile of a group when the tab changes underneath it.
static void CG_Moba_ShopMoveCursor( int dx, int dy )
{
	int slot = cgMoba.cursor;
	int col, row;

	if ( slot < 0 )
	{
		slot = 0;
	}
	else
	{
		col = ( slot % CG_MOBA_SHOP_COLS ) + dx;
		row = ( slot / CG_MOBA_SHOP_COLS ) + dy;

		if ( col < 0 )
		{
			col = CG_MOBA_SHOP_COLS - 1;
		}
		else if ( col >= CG_MOBA_SHOP_COLS )
		{
			col = 0;
		}

		if ( row < 0 )
		{
			row = CG_MOBA_SHOP_ROWS - 1;
		}
		else if ( row >= CG_MOBA_SHOP_ROWS )
		{
			row = 0;
		}

		slot = row * CG_MOBA_SHOP_COLS + col;
	}

	// stop on the last tile that really has an item, an empty tile would take
	// the cursor away from everything
	while ( slot > 0 && CG_Moba_ShopItem( slot ) < 0 )
	{
		slot--;
	}

	cgMoba.cursor = slot;
}

static void CG_Moba_DrawShop( void )
{
	static vec4_t colorWindow		= { 0.02f, 0.02f, 0.05f, 0.90f };
	static vec4_t colorBorder		= { 0.60f, 0.50f, 0.20f, 0.95f };
	static vec4_t colorTitle		= { 1.00f, 0.85f, 0.30f, 1.00f };
	static vec4_t colorText			= { 1.00f, 1.00f, 1.00f, 1.00f };
	static vec4_t colorDim			= { 0.65f, 0.65f, 0.65f, 1.00f };
	static vec4_t colorHint			= { 0.60f, 0.60f, 0.60f, 1.00f };
	static vec4_t colorPhase		= { 0.70f, 0.70f, 0.70f, 1.00f };
	static vec4_t colorUrgent		= { 1.00f, 0.35f, 0.35f, 1.00f };
	static vec4_t colorGold		= { 1.00f, 0.85f, 0.25f, 1.00f };
	static vec4_t colorPrice		= { 0.40f, 0.90f, 0.40f, 1.00f };
	static vec4_t colorPriceOwned	= { 0.35f, 0.50f, 0.35f, 1.00f };
	static vec4_t colorPricePoor	= { 0.95f, 0.30f, 0.30f, 1.00f };
	static vec4_t colorBought		= { 0.90f, 0.40f, 0.40f, 1.00f };
	static vec4_t colorTabOff		= { 0.14f, 0.14f, 0.18f, 0.90f };
	static vec4_t colorTabOn		= { 0.30f, 0.26f, 0.10f, 0.95f };
	static vec4_t colorTileBg		= { 0.10f, 0.10f, 0.14f, 0.85f };
	static vec4_t colorTileHover	= { 0.20f, 0.20f, 0.28f, 0.95f };
	static vec4_t colorIconBg		= { 0.03f, 0.03f, 0.06f, 0.90f };

	float x, y, textY, w;
	vec4_t phaseColor;
	int i, slot, item, secondsLeft, hover;

	if ( !CG_Moba_ShopOpen() )
	{
		return;
	}

	hover = CG_Moba_ShopTileAt( (float)cgs.cursorX, (float)cgs.cursorY );

	CG_FillRect( CG_MOBA_SHOP_X, CG_MOBA_SHOP_Y, CG_MOBA_SHOP_W, CG_MOBA_SHOP_H, colorWindow );
	CG_DrawRect( CG_MOBA_SHOP_X, CG_MOBA_SHOP_Y, CG_MOBA_SHOP_W, CG_MOBA_SHOP_H, 1.0f, colorBorder );

	// ---- title bar: name on the left, how long the phase lasts on the right ----
	CG_Text_Paint( CG_MOBA_SHOP_X + 10.0f, CG_MOBA_SHOP_Y + 4.0f, 0.8f, colorTitle,
		"MAGIC WANDS SHOP", 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_MEDIUM );

	secondsLeft = cgMoba.secondsLeft - ( cg.time - cgMoba.receivedAt ) / 1000;
	if ( secondsLeft < 0 )
	{
		secondsLeft = 0;
	}

	// the last seconds of the buy phase blink and turn red, a phase that runs
	// out silently is the easiest way to lose a purchase
	for ( i = 0; i < 4; i++ )
	{
		phaseColor[i] = ( secondsLeft <= 5 ) ? colorUrgent[i] : colorPhase[i];
	}

	if ( secondsLeft <= 5 )
	{
		phaseColor[3] = 0.35f + 0.65f * ( 0.5f + 0.5f * sin( cg.time * 0.009f ) );
	}

	{
		const char *clock = va( "%i s left", secondsLeft );

		w = (float)CG_Text_Width( clock, 0.7f, FONT_SMALL );
		CG_Text_Paint( CG_MOBA_SHOP_X + CG_MOBA_SHOP_W - 10.0f - w, CG_MOBA_SHOP_Y + 7.0f,
			0.7f, phaseColor, clock, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}

	// ---- gold and level ----
	textY = CG_MOBA_SHOP_Y + 21.0f;
	CG_Text_Paint( CG_MOBA_SHOP_X + 10.0f, textY, 0.75f,
		( cgMoba.gold > 0 ) ? colorGold : colorUrgent,
		va( "Gold: %i", cgMoba.gold ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_MEDIUM );
	CG_Text_Paint( CG_MOBA_SHOP_X + 90.0f, textY + 2.0f, 0.65f, colorDim,
		va( "level %i", cgMoba.level ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

	// ---- the three tabs ----
	for ( i = 0; i < CG_MOBA_ITEM_CATS; i++ )
	{
		qboolean on = ( i == cgMoba.category ) ? qtrue : qfalse;
		float tw = (float)CG_Text_Width( cgMobaItemCats[i], 0.7f, FONT_SMALL );

		x = CG_Moba_TabX( i );
		CG_FillRect( x, CG_MOBA_TAB_Y, CG_MOBA_TAB_W, CG_MOBA_TAB_H, on ? colorTabOn : colorTabOff );
		CG_DrawRect( x, CG_MOBA_TAB_Y, CG_MOBA_TAB_W, CG_MOBA_TAB_H, 1.0f, colorBorder );
		CG_Text_Paint( x + ( CG_MOBA_TAB_W - tw ) * 0.5f, CG_MOBA_TAB_Y + 1.0f, 0.7f,
			on ? colorTitle : colorDim, cgMobaItemCats[i], 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}

	// ---- the tiles of the open tab ----
	for ( slot = 0; slot < CG_MOBA_SHOP_SLOTS; slot++ )
	{
		const cgMobaItem_t *it;
		const char *price;
		float *tileBorder;
		qboolean owned, buyable;
		int held, c;

		item = CG_Moba_ShopItem( slot );
		if ( item < 0 )
		{
			continue;
		}

		it = &cgMobaItems[item];

		// how many copies the player carries right now, 0 when none
		held = 0;
		for ( c = 0; c < 2; c++ )
		{
			if ( cgMoba.slotItem[c] == item )
			{
				held = cgMoba.slotCount[c];
			}
		}

		owned = ( held > 0 ) ? qtrue : qfalse;
		buyable = CG_Moba_ShopBuyable( item );

		x = CG_Moba_ShopTileX( slot );
		y = CG_Moba_ShopTileY( slot );

		tileBorder = colorBorder;
		if ( hover == item || slot == cgMoba.cursor )
		{
			tileBorder = colorTitle;
		}
		else if ( !buyable )
		{
			tileBorder = colorDim;
		}

		CG_FillRect( x, y, CG_MOBA_SHOP_TILE_W, CG_MOBA_SHOP_TILE_H,
			( hover == item ) ? colorTileHover : colorTileBg );
		CG_DrawRect( x, y, CG_MOBA_SHOP_TILE_W, CG_MOBA_SHOP_TILE_H, 1.0f, tileBorder );

		// the same glyph the slot bar draws, so an item looks the same in the
		// shop and in the inventory. A dark box behind it keeps the icon from
		// disappearing into the tile.
		CG_FillRect( x + 6.0f, y + 8.0f, 20.0f, 20.0f, colorIconBg );
		CG_Moba_ItemIcon( x + 7.0f, y + 9.0f, 18.0f, item,
			( buyable || owned ) ? 1.0f : 0.40f );
		CG_DrawRect( x + 6.0f, y + 8.0f, 20.0f, 20.0f, 1.0f, tileBorder );

		// name and price share the first line, the price right aligned
		w = (float)CG_Text_Width( it->name, 0.7f, FONT_SMALL );
		price = ( owned && it->maxCount > 1 ) ?
			va( "%i/%ig", held, it->price ) :
			( owned ? "bought" : va( "%ig", it->price ) );
		CG_Text_Paint( x + 28.0f, y + 6.0f, 0.7f,
			owned ? colorPriceOwned : colorText, it->name, 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		CG_Text_Paint( x + CG_MOBA_SHOP_TILE_W - 8.0f -
				(float)CG_Text_Width( price, 0.7f, FONT_SMALL ),
			y + 6.0f, 0.7f,
			owned ? colorBought : ( buyable ? colorPrice : colorPricePoor ),
			price, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		// a longer name must never run into the price column
		if ( w + 40.0f > CG_MOBA_SHOP_TILE_W )
		{
			continue;
		}

		CG_Text_Paint( x + 28.0f, y + 22.0f, 0.6f,
			owned ? colorDim : colorText, it->desc, 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		// what a click on this tile would do, so no manual is needed
		CG_Text_Paint( x + 28.0f, y + 38.0f, 0.6f,
			owned ? colorDim : ( buyable ? colorPrice : colorPricePoor ),
			( owned && it->maxCount > 1 && held < it->maxCount ) ?
				"left click - buy another" :
			( owned ? "in your slots" :
				( buyable ? "left click - buy" :
					( cgMoba.slotItem[0] >= 0 && cgMoba.slotItem[1] >= 0 ?
						"both slots are full" : "not enough gold" ) ) ), 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}

	// ---- bottom line ----
	CG_Text_Paint( CG_MOBA_SHOP_X + 10.0f, CG_MOBA_SHOP_Y + CG_MOBA_SHOP_H - 18.0f, 0.7f,
		colorHint, "you own two items - they sit in the C and V slots, press the "
		"slot key to use one",
		0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
}

//=========================================================================
// Everything the mod draws, in the order the panels stack: the hero window and
// the shop are the two modal windows and only one of them can be open, the
// ability bar lives in the buy and the fight phase and the pointer goes on top
// of every window that has the mouse.
//=========================================================================
//=========================================================================
// Round clock at the top of the screen. The buy freeze counts down first,
// then the fight clock takes over. Both ride on the shop state (phase and
// seconds), which the server pushes once a second.
//=========================================================================
static void CG_Moba_DrawRoundTimer( void )
{
	static vec4_t colorBuy		= { 0.40f, 0.90f, 0.40f, 1.00f };
	static vec4_t colorFight	= { 1.00f, 0.85f, 0.30f, 1.00f };
	static vec4_t colorUrgent	= { 1.00f, 0.30f, 0.30f, 1.00f };
	vec4_t color;
	const char *label;
	char buf[32];
	float w;
	int secs, mins;

	if ( cgMoba.phase != CG_MOBA_PHASE_BUY && cgMoba.phase != CG_MOBA_PHASE_FIGHT )
	{
		return;
	}

	secs = cgMoba.secondsLeft - ( cg.time - cgMoba.receivedAt ) / 1000;
	if ( secs < 0 )
	{
		secs = 0;
	}

	mins = secs / 60;
	secs %= 60;

	if ( cgMoba.phase == CG_MOBA_PHASE_BUY )
	{
		label = "BUY";
		Com_sprintf( buf, sizeof( buf ), "%s  %i:%02i", label, mins, secs );
	}
	else
	{
		Com_sprintf( buf, sizeof( buf ), "%i:%02i", mins, secs );
	}

	if ( secs <= 10 )
	{
		int i;

		for ( i = 0; i < 4; i++ )
		{
			color[i] = colorUrgent[i];
		}
	}
	else
	{
		int i;

		for ( i = 0; i < 4; i++ )
		{
			color[i] = ( cgMoba.phase == CG_MOBA_PHASE_BUY ) ? colorBuy[i] : colorFight[i];
		}
	}

	if ( secs <= 5 )
	{
		color[3] = 0.35f + 0.65f * ( 0.5f + 0.5f * sin( cg.time * 0.009f ) );
	}

	w = (float)CG_Text_Width( buf, 0.9f, FONT_MEDIUM );
	CG_Text_Paint( 320.0f - w * 0.5f, 10.0f, 0.9f, color, buf, 0, 0,
		ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_MEDIUM );
}

void CG_Moba_Draw( void )
{
	static vec4_t colorHint	= { 0.65f, 0.65f, 0.65f, 1.0f };
	static vec4_t colorPhase = { 0.70f, 0.70f, 0.70f, 1.00f };
	static int noStatePrintedAt = 0;

	// Nothing here can draw a single pixel before the server sent "mobaShop":
	// no state means no shop, no hero board, no pick and no hint, which is
	// exactly what a server with the mod switched off looks like from the
	// client. Say so in the console instead of leaving an empty screen, and
	// repeat it every few seconds so a late start is picked up as well.
	if ( cg_moba.integer && !cgMoba.received && cg.time - noStatePrintedAt > 5000 )
	{
		noStatePrintedAt = cg.time;
		trap->Print( va( "MOBA: still no state from the server (cg_moba %i, "
			"gametype %i) - MOBA is off there, or the wrong game dll\n",
			cg_moba.integer, cgs.gametype ) );
	}

	// the catcher is serviced here as well, a window that is not on screen has
	// to give the mouse back
	CG_Moba_DrawDraft();
	CG_Moba_DrawShop();

	// the round clock sits at the very top, above every panel
	CG_Moba_DrawRoundTimer();

	// the cursor goes last, on top of every panel the window drew
	CG_Moba_DrawCursor();

	CG_Moba_Input();
	CG_Moba_AbilityInput();
	CG_Moba_ItemInput();
	CG_Moba_FlameInput();

	// the ability bar belongs to the bottom of the screen, the shop window to
	// the middle of it, so they can be on screen together
	CG_Moba_DrawItemSlots();
	CG_Moba_DrawAbilityBar();

	if ( !CG_Moba_ShopOpen() )
	{
		// while the buy phase runs, keep the shop discoverable
		if ( CG_Moba_CanShop() )
		{
			CG_Text_Paint( 16.0f, 82.0f, 0.6f, colorHint, "B - open the shop", 0, 0,
				ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}
		else if ( cg.time < cgMoba.noticeUntil )
		{
			CG_Text_Paint( 16.0f, 82.0f, 0.6f, colorPhase,
				"the shop only opens during the buy phase", 0, 0,
				ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}
	}
}
