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
	vec4_t		chip;			// tile colour, groups the items inside a tab
} cgMobaItem_t;

static const char *cgMobaItemCats[] = { "DEFENCE", "ATTACK", "CONSUMABLES" };
#define CG_MOBA_ITEM_CATS	( (int)( sizeof( cgMobaItemCats ) / sizeof( cgMobaItemCats[0] ) ) )

// The order is the order of mobaItems on the server, because the index of a row
// is the bit the item mask uses, so the two tables may never be sorted apart.
static const cgMobaItem_t cgMobaItems[] = {
	{ "Sturdy Armor",	250,	"+50 armor",					0,	{ 0.30f, 0.50f, 0.80f, 0.95f } },
	{ "Med Kit",		200,	"+100 health",				2,	{ 0.75f, 0.25f, 0.25f, 0.95f } },
	{ "Rage Rune",		300,	"+20% damage",				1,	{ 0.85f, 0.45f, 0.15f, 0.95f } },
	{ "Heavy Plate",	500,	"+100 armor, +50 health",		0,	{ 0.25f, 0.45f, 0.70f, 0.95f } },
	{ "Power Crystal",	650,	"+50 health, +40% damage",	1,	{ 0.60f, 0.30f, 0.75f, 0.95f } },
	{ "Shadow Cloak",	400,	"+30 armor, +15% damage",		0,	{ 0.25f, 0.60f, 0.40f, 0.95f } }
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
#define CG_MOBA_ABILITIES	4

typedef struct {
	int			heroId;
	int			cooldown[CG_MOBA_ABILITIES];	// ms left at the moment of the push
	int			level[CG_MOBA_ABILITIES];
	int			receivedAt;
	qboolean	received;
	int			loggedHero;		// -2 before the first push, so the first one logs
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

typedef struct {
	int			bannedMask;		// one bit per hero, index into mobaHeroTable
	int			redMask;
	int			blueMask;
	int			takenMask;		// heroes a player has already taken
	int			action;			// CG_MOBA_ACT_*, what the current step is
	int			canAct;			// only the server may answer this
	int			secondsLeft;
	int			receivedAt;
	int			myHero;			// -1 while the player has no hero
	int			myTeam;			// 0 none, 1 red, 2 blue (team_t on the server)
	int			step, steps;
	int			phase;			// shop phase the draft state belongs to
	qboolean	received;
	qboolean	logged;			// one time confirmation in the client log
	qboolean	hadCatcher;		// the window currently owns the mouse
	qboolean	dismissed;		// only the !draft command clears this
	int			cursor;			// keyboard cursor, -1 = nothing
	int			selected;		// hero the confirm button would act on
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
	int pool;

	if ( heroId < 0 || heroId >= MOBA_MAX_HEROES || !cgMobaDraft.canAct ||
		cgMobaDraft.action == CG_MOBA_ACT_NONE )
	{
		return qfalse;
	}

	if ( cgMobaDraft.bannedMask & ( 1 << heroId ) )
	{
		return qfalse;
	}

	if ( cgMobaDraft.takenMask & ( 1 << heroId ) )
	{
		return qfalse;
	}

	if ( cgMoba.phase == CG_MOBA_PHASE_DRAFT )
	{
		int gone;

		// the captains take heroes off the board, so a hero is gone as soon as it
		// sits in one of the two masks. That is a union, not an intersection: a
		// hero only one captain has taken must not be offered to the other.
		gone = cgMobaDraft.redMask | cgMobaDraft.blueMask;

		return ( gone & ( 1 << heroId ) ) ? qfalse : qtrue;
	}

	// the assign stage hands out heroes out of the pool of the own team
	pool = ( cgMobaDraft.myTeam == 1 ) ? cgMobaDraft.redMask :
		( cgMobaDraft.myTeam == 2 ) ? cgMobaDraft.blueMask : 0;

	return ( pool & ( 1 << heroId ) ) ? qtrue : qfalse;
}

//=========================================================================
// Server command handler: "mobaDraft <banned> <red> <blue> <taken> <action>
// <canAct> <seconds> <myHero> <myTeam> <step> <steps>". Thirty heroes fit into
// one int, so the whole board travels in a single command and the window never
// has to ask the server for a second opinion on what is already banned or
// picked. <taken> is what the team pool masks cannot say: who already has a
// hero out of the pool.
//=========================================================================
void CG_Moba_DraftCommand_f( void )
{
	char buf[128], *p;
	int v[11], i;

	if ( cg_moba.integer == 0 || !CG_Argv( 1 ) || !CG_Argv( 1 )[0] )
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

		// A new phase always clears the choice, and the mouse bit decides how the
		// window is currently held: still set means it kept the mouse across the
		// phase change, clear means it has to take it again.
		cgMobaDraft.hadCatcher = ( trap->Key_GetCatcher() & KEYCATCH_CGAME ) ? qtrue : qfalse;
	}

	cgMobaDraft.bannedMask = v[0];
	cgMobaDraft.redMask = v[1];
	cgMobaDraft.blueMask = v[2];
	cgMobaDraft.takenMask = v[3];
	cgMobaDraft.action = v[4];
	cgMobaDraft.canAct = v[5];
	cgMobaDraft.secondsLeft = v[6];
	cgMobaDraft.receivedAt = cg.time;
	cgMobaDraft.myHero = v[7];
	cgMobaDraft.myTeam = v[8];
	cgMobaDraft.step = v[9];
	cgMobaDraft.steps = v[10];
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
static int CG_Moba_TileAt( float mx, float my )
{
	float fx, fy;
	int col, row;

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

	return row * CG_MOBA_COLS + col;
}

static const char *CG_Moba_HeroState( int heroId )
{
	if ( cgMobaDraft.bannedMask & ( 1 << heroId ) )
	{
		return "banned";
	}
	if ( cgMobaDraft.redMask & ( 1 << heroId ) )
	{
		return "red team";
	}
	if ( cgMobaDraft.blueMask & ( 1 << heroId ) )
	{
		return "blue team";
	}
	if ( cgMobaDraft.myHero == heroId )
	{
		return "yours";
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
		Q_strncpyz( cgMobaDraft.notice,
			( cgMoba.phase == CG_MOBA_PHASE_DRAFT ) ? "wait for your turn" : "you already have a hero",
			sizeof( cgMobaDraft.notice ) );
		cgMobaDraft.noticeUntil = cg.time + 2500;
		return;
	}

	if ( cgMobaDraft.bannedMask & ( 1 << heroId ) )
	{
		Q_strncpyz( cgMobaDraft.notice, "that hero is banned", sizeof( cgMobaDraft.notice ) );
		cgMobaDraft.noticeUntil = cg.time + 2500;
		return;
	}

	if ( cgMobaDraft.takenMask & ( 1 << heroId ) )
	{
		Q_strncpyz( cgMobaDraft.notice, "that hero is already taken", sizeof( cgMobaDraft.notice ) );
		cgMobaDraft.noticeUntil = cg.time + 2500;
		return;
	}

	if ( cgMoba.phase == CG_MOBA_PHASE_DRAFT &&
		( cgMobaDraft.redMask & ( 1 << heroId ) || cgMobaDraft.blueMask & ( 1 << heroId ) ) )
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
static qboolean CG_Moba_BarWanted( void );
static int CG_Moba_BarAt( float mx, float my );
static void CG_Moba_CastAbility( int slot );

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
		( cgMoba.phase == CG_MOBA_PHASE_DRAFT_ASSIGN ) ? "CHOOSE YOUR HERO" : "CAPTAIN DRAFT",
		0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_MEDIUM );

	secondsLeft = cgMobaDraft.secondsLeft - ( cg.time - cgMobaDraft.receivedAt ) / 1000;
	if ( secondsLeft < 0 )
	{
		secondsLeft = 0;
	}

	if ( cgMoba.phase == CG_MOBA_PHASE_DRAFT )
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
		qboolean banned = ( cgMobaDraft.bannedMask & ( 1 << i ) ) ? qtrue : qfalse;
		qboolean red = ( cgMobaDraft.redMask & ( 1 << i ) ) ? qtrue : qfalse;
		qboolean blue = ( cgMobaDraft.blueMask & ( 1 << i ) ) ? qtrue : qfalse;
		float nameW;
		int c;

		col = i % CG_MOBA_COLS;
		row = i / CG_MOBA_COLS;
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

		nameW = (float)CG_Text_Width( hero->name, 0.62f, FONT_SMALL );
		CG_Text_Paint( x + ( CG_MOBA_TILE_W - nameW ) * 0.5f, y + 14.0f, 0.62f,
			banned ? colorBannedText : colorText, hero->name,
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		CG_Text_Paint( x + 5.0f, y + 32.0f, 0.55f, banned ? colorBannedText : roleColor,
			hero->role, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		state = CG_Moba_HeroState( i );
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
		else if ( state )
		{
			CG_Text_Paint( x + CG_MOBA_TILE_W - 26.0f, y + 32.0f, 0.55f, colorGold,
				"you", 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}
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

		CG_Text_Paint( x, textY, 0.58f, colorDim,
			va( "Damage %i (+%i/lv)", hero->baseDamage, hero->damagePerLevel ), 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		textY += 12.0f;

		state = CG_Moba_HeroState( active );
		if ( state )
		{
			CG_Text_Paint( x, textY, 0.58f,
				( cgMobaDraft.bannedMask & ( 1 << active ) ) ? colorRed : colorDim,
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
// Server command handler: "mobaShop <phase> <seconds> <gold> <mask> <level>".
// A new phase reopens the shop, so ESC only hides it for the current one.
//=========================================================================
void CG_Moba_ServerCommand_f( void )
{
	char buf[64], *p;
	int v[5], i;

	if ( !CG_Argv( 1 ) || !CG_Argv( 1 )[0] || cg_moba.integer == 0 )
	{
		return;
	}

	Q_strncpyz( buf, CG_Argv( 1 ), sizeof( buf ) );
	p = buf;
	for ( i = 0; i < 5; i++ )
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
	cgMoba.gold = v[2];
	cgMoba.itemMask = v[3];
	cgMoba.level = v[4];
	cgMoba.received = qtrue;

	if ( !cgMoba.logged )
	{
		cgMoba.logged = qtrue;
		trap->Print( va( "MOBA: shop state received: %s (cg_moba %i)\n",
			buf, cg_moba.integer ) );
	}
}

//=========================================================================
// Ability bar
//
// The four abilities are on Q, E, C and V. The letters live in moba.cfg as
// binds that write a token into cg_mobaAbility, because the JKA client turns
// keys into buttons before the cgame ever sees them (see CG_Moba_HandleToken).
// moba.cfg is where a player rebinds them, and the bar shows the same letters,
// so the picture on the screen and the config never disagree.
//
// The icons are drawn out of plain rectangles. JKA ships no art for a moba
// kit, and the stock force icons are square jpegs without an alpha channel,
// which would come out as black boxes on a dark panel.
//=========================================================================
static const char *cgMobaAbilityKeys[CG_MOBA_ABILITIES] = { "Q", "E", "C", "V" };

// The bar is a corner of the screen, not the middle of it: a quarter of the
// height of a box would cover the fight the player is trying to watch.
#define CG_MOBA_BAR_SIZE		22.0f
#define CG_MOBA_BAR_GAP		4.0f
// high enough that a slot and the line above it both stay on screen
#define CG_MOBA_BAR_Y		( 480.0f - 44.0f )

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

	if ( token[0] >= '1' && token[0] <= '4' && !token[1] )
	{
		CG_Moba_CastAbility( token[0] - '1' );
	}

	// clear the token again so the same key can fire the next one
	trap->SendConsoleCommand( "set cg_mobaAbility 0\n" );
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
}

//=========================================================================
// Server command handler for the ability bar:
// "mobaAbilities heroId cd0 cd1 cd2 cd3 lv0 lv1 lv2 lv3".
//
// The hero id comes first so the bar knows whose four abilities it draws, and
// the cooldowns are milliseconds rather than seconds: a 6 second cooldown that
// ticks in whole seconds would sit at "1" for a full second and then jump.
//=========================================================================
void CG_Moba_AbilitiesCommand_f( void )
{
	char buf[64], *p;
	int v[9], i;

	if ( !CG_Argv( 1 ) || !CG_Argv( 1 )[0] || cg_moba.integer == 0 )
	{
		return;
	}

	Q_strncpyz( buf, CG_Argv( 1 ), sizeof( buf ) );
	p = buf;
	for ( i = 0; i < 9; i++ )
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
	}

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
// depends on it, the click always asks the server, which owns the price.
//=========================================================================
static qboolean CG_Moba_ShopBuyable( int item )
{
	if ( item < 0 || item >= CG_MOBA_NUM_ITEMS )
	{
		return qfalse;
	}

	if ( cgMoba.itemMask & ( 1 << item ) )
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
		vec4_t chip;
		float *tileBorder;
		qboolean owned, buyable;
		int c;

		item = CG_Moba_ShopItem( slot );
		if ( item < 0 )
		{
			continue;
		}

		it = &cgMobaItems[item];
		owned = ( cgMoba.itemMask & ( 1 << item ) ) ? qtrue : qfalse;
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

		// the colour of the group: the chip turns grey once the item is owned
		for ( c = 0; c < 4; c++ )
		{
			chip[c] = it->chip[c];
		}

		if ( owned )
		{
			chip[0] *= 0.35f;
			chip[1] *= 0.35f;
			chip[2] *= 0.35f;
		}

		CG_FillRect( x + 6.0f, y + 8.0f, 16.0f, 16.0f, chip );
		CG_DrawRect( x + 6.0f, y + 8.0f, 16.0f, 16.0f, 1.0f, tileBorder );

		// name and price share the first line, the price right aligned
		w = (float)CG_Text_Width( it->name, 0.7f, FONT_SMALL );
		price = owned ? "bought" : va( "%ig", it->price );
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
			owned ? "in your inventory" :
				( buyable ? "left click - buy" : "not enough gold" ), 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}

	// ---- bottom line ----
	CG_Text_Paint( CG_MOBA_SHOP_X + 10.0f, CG_MOBA_SHOP_Y + CG_MOBA_SHOP_H - 18.0f, 0.7f,
		colorHint, "left click - buy    arrows and Enter work too    B or ESC - close",
		0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
}

//=========================================================================
// Everything the mod draws, in the order the panels stack: the hero window and
// the shop are the two modal windows and only one of them can be open, the
// ability bar lives in the buy and the fight phase and the pointer goes on top
// of every window that has the mouse.
//=========================================================================
void CG_Moba_Draw( void )
{
	static vec4_t colorHint	= { 0.65f, 0.65f, 0.65f, 1.0f };
	static vec4_t colorPhase = { 0.70f, 0.70f, 0.70f, 1.00f };

	// the catcher is serviced here as well, a window that is not on screen has
	// to give the mouse back
	CG_Moba_DrawDraft();
	CG_Moba_DrawShop();

	// the cursor goes last, on top of every panel the window drew
	CG_Moba_DrawCursor();

	CG_Moba_Input();
	CG_Moba_AbilityInput();

	// the ability bar belongs to the bottom of the screen, the shop window to
	// the middle of it, so they can be on screen together
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
