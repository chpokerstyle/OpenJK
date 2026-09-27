/*
===========================================================================
MagicWands MOBA mod - OpenJK cgame (on-screen shop)
===========================================================================
The server pushes "mobaShop phase seconds gold itemMask level" to the client
once per change. That is the only thing this file needs to know, so the panel
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
// The key column mirrors the binds in moba.cfg: the JKA client never hands key
// codes to the cgame, so a bind writes a token into cg_mobaKey instead. The
// keys are the ones the default JKA layout leaves free, digits and numpad stay
// on weapons, movement and force powers.
//=========================================================================
typedef struct {
	const char	*name;
	int			price;
	const char	*desc;
	const char	*key;			// moba.cfg bind that buys this slot
	vec4_t		chip;			// keycap colour, groups the items by kind
} cgMobaItem_t;

static const cgMobaItem_t cgMobaItems[] = {
	{ "Sturdy Armor",	250,	"+50 armor",					"G",	{ 0.30f, 0.50f, 0.80f, 0.95f } },
	{ "Med Kit",		200,	"+100 health",				"H",	{ 0.75f, 0.25f, 0.25f, 0.95f } },
	{ "Rage Rune",		300,	"+20% damage",				"J",	{ 0.85f, 0.45f, 0.15f, 0.95f } },
	{ "Heavy Plate",	500,	"+100 armor, +50 health",		"N",	{ 0.25f, 0.45f, 0.70f, 0.95f } },
	{ "Power Crystal",	650,	"+50 health, +40% damage",	"X",	{ 0.60f, 0.30f, 0.75f, 0.95f } },
	{ "Shadow Cloak",	400,	"+30 armor, +15% damage",		";",	{ 0.25f, 0.60f, 0.40f, 0.95f } }
};

#define CG_MOBA_NUM_ITEMS	( (int)( sizeof( cgMobaItems ) / sizeof( cgMobaItems[0] ) ) )

// mirrors mobaPhase_t from g_moba.h, keep both in sync: LOBBY 0, DRAFT 1,
// DRAFT_ASSIGN 2, BUY 3, FIGHT 4, ROUNDEND 5
#define CG_MOBA_PHASE_DRAFT			1
#define CG_MOBA_PHASE_DRAFT_ASSIGN	2
#define CG_MOBA_PHASE_BUY			3

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
	qboolean	open;				// the player opened the shop with B
	qboolean	received;
	qboolean	logged;			// one time confirmation in the client log
} cgMobaState_t;

static cgMobaState_t cgMoba;

//=========================================================================
// Hero select window
//
// The window is a modal panel: it takes the mouse from the game (see
// CG_Moba_DraftCatcher) so the player can look at the board instead of at the
// world, and it draws every hero of mobaHeroTable with the ban and pick state
// the server pushed. Nothing here decides anything, a click only turns into
// the same !pick / !ban command a player would type in chat.
//=========================================================================
#define CG_MOBA_WIN_X			20.0f
#define CG_MOBA_WIN_Y			28.0f
#define CG_MOBA_WIN_W			600.0f
#define CG_MOBA_WIN_H			404.0f

#define CG_MOBA_COLS			5
#define CG_MOBA_ROWS			6
#define CG_MOBA_TILE_W			76.0f
#define CG_MOBA_TILE_H			52.0f
#define CG_MOBA_TILE_GAP		3.0f

#define CG_MOBA_GRID_X			( CG_MOBA_WIN_X + 8.0f )
#define CG_MOBA_GRID_Y			( CG_MOBA_WIN_Y + 48.0f )
#define CG_MOBA_INFO_X			( CG_MOBA_GRID_X + CG_MOBA_COLS * ( CG_MOBA_TILE_W + CG_MOBA_TILE_GAP ) + 8.0f )
#define CG_MOBA_INFO_W			( CG_MOBA_WIN_X + CG_MOBA_WIN_W - 8.0f - CG_MOBA_INFO_X )

#define CG_MOBA_ACT_NONE		0
#define CG_MOBA_ACT_BAN			1
#define CG_MOBA_ACT_PICK		2

typedef struct {
	int			bannedMask;		// one bit per hero, index into mobaHeroTable
	int			redMask;
	int			blueMask;
	int			action;			// CG_MOBA_ACT_*, what the current step is
	int			canAct;			// only the server may answer this
	int			secondsLeft;
	int			receivedAt;
	int			myHero;			// -1 while the player has no hero
	int			step, steps;
	int			phase;			// shop phase the draft state belongs to
	qboolean	received;
	qboolean	logged;			// one time confirmation in the client log
	qboolean	hadCatcher;		// the window currently owns the mouse
	qboolean	dismissed;		// ESC closed it until the next phase
	int			cursor;			// keyboard cursor, -1 = nothing
	char		notice[64];
	int			noticeUntil;
} cgMobaDraft_t;

static cgMobaDraft_t cgMobaDraft;

//=========================================================================
// Server command handler: "mobaDraft <banned> <red> <blue> <action> <canAct>
// <seconds> <myHero> <step> <steps>". Thirty heroes fit into one int, so the
// whole board travels in a single command and the window never has to ask the
// server for a second opinion on what is already banned or picked.
//=========================================================================
void CG_Moba_DraftCommand_f( void )
{
	char buf[128], *p;
	int v[9], i;

	if ( cg_moba.integer == 0 || !CG_Argv( 1 ) || !CG_Argv( 1 )[0] )
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

	// The shop state carries the phase and arrives first (MOBA_PushShopState is
	// called before MOBA_PushDraftState), so a phase change can be seen here.
	// It is the only moment the window may open itself again: a push that only
	// ticks the clock down must not undo an ESC.
	if ( cgMoba.phase != cgMobaDraft.phase )
	{
		cgMobaDraft.phase = cgMoba.phase;
		cgMobaDraft.dismissed = qfalse;
		cgMobaDraft.cursor = -1;

		// A new phase always gives the player a second chance, but the mouse bit
		// decides how the window is currently held: still set means it stayed
		// open across the phase change, clear means the player closed it with
		// ESC and only the ESC has to be forgotten.
		cgMobaDraft.hadCatcher = ( trap->Key_GetCatcher() & KEYCATCH_CGAME ) ? qtrue : qfalse;
	}

	cgMobaDraft.bannedMask = v[0];
	cgMobaDraft.redMask = v[1];
	cgMobaDraft.blueMask = v[2];
	cgMobaDraft.action = v[3];
	cgMobaDraft.canAct = v[4];
	cgMobaDraft.secondsLeft = v[5];
	cgMobaDraft.receivedAt = cg.time;
	cgMobaDraft.myHero = v[6];
	cgMobaDraft.step = v[7];
	cgMobaDraft.steps = v[8];
	cgMobaDraft.received = qtrue;

	if ( !cgMobaDraft.logged )
	{
		cgMobaDraft.logged = qtrue;
		trap->Print( va( "MOBA: draft state received: %s\n", buf ) );
	}
}

//=========================================================================
// The window exists while the server is in one of the two draft phases and the
// player did not close it.
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

// The JKA client only hands key codes to the cgame while KEYCATCH_CGAME is set
// (cl_keys.cpp CL_KeyEvent) and only feeds mouse deltas to CG_MouseEvent under
// the same bit (cl_input.cpp IN_MouseMove). The window therefore has to hold
// that bit, and it has to let it go again when it closes. ESC is the one key
// the client eats itself: it clears the bit and calls CG_EventHandling before
// the cgame ever sees it, so losing the bit while the window wanted it means
// the player closed the window, not that the server changed its mind.
static void CG_Moba_DraftCatcher( void )
{
	int catcher = trap->Key_GetCatcher();
	qboolean want = CG_Moba_DraftWanted();

	if ( want && !( catcher & KEYCATCH_CGAME ) )
	{
		if ( cgMobaDraft.hadCatcher )
		{
			cgMobaDraft.dismissed = qtrue;
			return;
		}

		trap->Key_SetCatcher( catcher | KEYCATCH_CGAME );
		cgMobaDraft.hadCatcher = qtrue;
	}
	else if ( !want && ( catcher & KEYCATCH_CGAME ) && cgMobaDraft.hadCatcher )
	{
		trap->Key_SetCatcher( catcher & ~KEYCATCH_CGAME );
		cgMobaDraft.hadCatcher = qfalse;
	}
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
// Turns a click into the chat command the player would have typed. The server
// owns every rule, this only avoids the pointless round trip for a hero that is
// plainly gone already.
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
}

//=========================================================================
// Input. Runs before the cgame decides what a key is for, because a living
// local player would otherwise swallow every key and every mouse button.
// Returns qtrue when the window used the key, so the game never acts on it.
//=========================================================================
qboolean CG_Moba_KeyEvent( int key, qboolean down )
{
	int hero;

	if ( !down || !CG_Moba_DraftWanted() )
	{
		return qfalse;
	}

	// the mouse wins over the keyboard cursor while it is on the board, so a
	// click always hits the tile the player is looking at
	hero = CG_Moba_TileAt( (float)cgs.cursorX, (float)cgs.cursorY );

	switch ( key )
	{
	case A_MOUSE1:
	case A_ENTER:
		CG_Moba_DraftAct( ( hero >= 0 ) ? hero : cgMobaDraft.cursor );
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
		return qtrue;

	case A_0:
		CG_Moba_DraftAct( 9 );
		return qtrue;

	case A_1: case A_2: case A_3: case A_4: case A_5:
	case A_6: case A_7: case A_8: case A_9:
		// the number on the tile is the number of !pick, so the keys and the
		// board can never mean different heroes
		CG_Moba_DraftAct( key - A_1 );
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

	vec4_t roleColor, bg, border;
	float x, y, textY;
	int i, col, row, secondsLeft, hover, active;
	const char *state;

	// the catcher has to be serviced every frame, also while the window is
	// closed, otherwise the mouse would stay locked in a panel nobody can see
	CG_Moba_DraftCatcher();

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
	active = ( hover >= 0 ) ? hover : cgMobaDraft.cursor;

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

	// ---- bottom line: what a click would do, or why nothing happened ----
	// the notice has to sit below both panels, otherwise the early return of the
	// "no hero under the mouse" case would swallow the feedback for a bad click
	if ( cg.time < cgMobaDraft.noticeUntil && cgMobaDraft.notice[0] )
	{
		CG_Text_Paint( CG_MOBA_WIN_X + 8.0f, CG_MOBA_WIN_Y + CG_MOBA_WIN_H - 15.0f, 0.62f,
			colorGold, cgMobaDraft.notice, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}
	else
	{
		CG_Text_Paint( CG_MOBA_WIN_X + 8.0f, CG_MOBA_WIN_Y + CG_MOBA_WIN_H - 15.0f, 0.6f,
			colorHint,
			( cgMoba.phase == CG_MOBA_PHASE_DRAFT ) ?
				"left click: ban  -  ESC: close window" :
				"left click: take the hero  -  ESC: close window",
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

			CG_Text_Paint( x, textY, 0.6f, colorText,
				va( "%i. %s", a + 1, ab->name ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
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
// code and a cgame side key hook can never see "B" or "1". Instead the binds
// in moba.cfg write a token into the cg_mobaKey cvar and the panel reacts to
// the change once per frame:
//
//   b  - open or close the shop
//   1..6 - buy that slot and close the shop again
//
// Escape never reaches a bind: cl_keys.cpp handles it before CL_ParseBinding
// and opens the game menu instead (that is also why no token can close the
// panel from the client side). B closes it again.
//=========================================================================
static void CG_Moba_HandleToken( const char *token )
{
	if ( !Q_stricmp( token, "b" ) )
	{
		if ( CG_Moba_CanShop() )
		{
			cgMoba.open = !cgMoba.open;
		}
		else
		{
			// say why nothing happened instead of a dead key press
			cgMoba.noticeUntil = cg.time + 2500;
		}

		return;
	}

	if ( token[0] >= '1' && token[0] <= '6' && !token[1] )
	{
		if ( CG_Moba_ShopOpen() )
		{
			// the vm console command queue has no separator, so the newline has
			// to be part of every command the cgame sends (cl_cgame.cpp
			// Cbuf_AddText), otherwise it merges with the next one
			//
			// "cmd say ..." instead of a plain "say ...": the client only
			// forwards a console command to the server after the cgame, the ui
			// and the cvar lookups have all declined it, and "cmd" skips that
			// chain completely (cl_main.cpp CL_ForwardToServer_f)
			trap->SendConsoleCommand( va( "cmd say !buy %c\n", token[0] ) );
			cgMoba.open = qfalse;	// one item per shop opening
		}

		return;
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
// Draws the shop in the upper left corner, over the HUD but under the chat
// box. Prices are right aligned with CG_Text_Width because the JKA font is
// proportional and would break a fixed column layout. The hero window owns the
// other phases and is drawn first, the two can never be on screen together.
//=========================================================================
void CG_Moba_Draw( void )
{
	static vec4_t colorBackground = { 0.0f, 0.0f, 0.0f, 0.55f };
	static vec4_t colorBorder = { 0.6f, 0.5f, 0.2f, 0.9f };
	static vec4_t colorTitle = { 1.0f, 0.85f, 0.3f, 1.0f };
	static vec4_t colorPhase = { 0.7f, 0.7f, 0.7f, 1.0f };
	static vec4_t colorUrgent = { 1.0f, 0.35f, 0.35f, 1.0f };
	static vec4_t colorNormal = { 1.0f, 1.0f, 1.0f, 1.0f };
	static vec4_t colorPoor = { 0.65f, 0.55f, 0.55f, 1.0f };
	static vec4_t colorOwned = { 0.5f, 0.5f, 0.5f, 1.0f };
	static vec4_t colorPrice = { 0.4f, 0.9f, 0.4f, 1.0f };
	static vec4_t colorPriceOwned = { 0.35f, 0.5f, 0.35f, 1.0f };
	static vec4_t colorPricePoor = { 0.95f, 0.3f, 0.3f, 1.0f };
	static vec4_t colorBought = { 0.9f, 0.4f, 0.4f, 1.0f };
	static vec4_t colorGold = { 1.0f, 0.85f, 0.25f, 1.0f };
	static vec4_t colorHint = { 0.65f, 0.65f, 0.65f, 1.0f };
	static vec4_t colorCap = { 1.0f, 1.0f, 1.0f, 1.0f };

	const float panelWidth = 272.0f;
	const float rowHeight = 15.0f;
	const float titleHeight = 18.0f;
	const float keySize = 11.0f;
	float x = 16.0f, y = 80.0f, w = panelWidth;
	float panelHeight, textY, priceX, tagX;
	int i, secondsLeft;

	// the catcher is serviced here as well, a window that is not on screen has
	// to give the mouse back
	CG_Moba_DrawDraft();

	CG_Moba_Input();

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

		return;
	}

	panelHeight = titleHeight + rowHeight * ( CG_MOBA_NUM_ITEMS + 2 ) + 8.0f;

	CG_FillRect( x, y, w, panelHeight, colorBackground );
	CG_DrawRect( x, y, w, panelHeight, 1.0f, colorBorder );

	textY = y + 4.0f;
	CG_Text_Paint( x + 8.0f, textY, 0.8f, colorTitle, "MAGIC WANDS SHOP", 0, 0,
		ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_MEDIUM );

	secondsLeft = cgMoba.secondsLeft - ( cg.time - cgMoba.receivedAt ) / 1000;
	if ( secondsLeft < 0 )
	{
		secondsLeft = 0;
	}

	// the last seconds of the buy phase blink and turn red, a phase that runs
	// out silently is the easiest way to lose a purchase
	{
		vec4_t phaseColor;
		qboolean urgent = ( secondsLeft <= 5 ) ? qtrue : qfalse;
		int c;

		for ( c = 0; c < 4; c++ )
		{
			phaseColor[c] = urgent ? colorUrgent[c] : colorPhase[c];
		}

		if ( urgent )
		{
			phaseColor[3] = 0.35f + 0.65f * ( 0.5f + 0.5f * sin( cg.time * 0.009f ) );
		}

		textY += titleHeight;
		CG_Text_Paint( x + 8.0f, textY, 0.7f, phaseColor,
			va( "Buy phase - %i s left - level %i", secondsLeft, cgMoba.level ),
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
	}

	textY += rowHeight;

	// the price column sits 46px in from the right edge, the bought tag 8px
	priceX = x + w - 46.0f;
	tagX = x + w - 8.0f;

	for ( i = 0; i < CG_MOBA_NUM_ITEMS; i++ )
	{
		const cgMobaItem_t *item = &cgMobaItems[i];
		qboolean owned = ( cgMoba.itemMask & ( 1 << i ) ) ? qtrue : qfalse;
		qboolean poor = ( !owned && item->price > cgMoba.gold ) ? qtrue : qfalse;
		const char *label = item->name;
		const char *price = va( "%ig", item->price );
		float labelW = (float)CG_Text_Width( label, 0.7f, FONT_SMALL );
		float priceW = (float)CG_Text_Width( price, 0.7f, FONT_SMALL );
		float keyW = (float)CG_Text_Width( item->key, 0.7f, FONT_SMALL );
		float *rowColor = owned ? colorOwned : ( poor ? colorPoor : colorNormal );
		vec4_t chipColor;
		int c;

		for ( c = 0; c < 4; c++ )
		{
			chipColor[c] = item->chip[c];
		}

		if ( labelW + priceW + 76.0f > w )
		{
			continue;	// never let a longer name overlap the price column
		}

		if ( owned )
		{
			chipColor[0] *= 0.35f;
			chipColor[1] *= 0.35f;
			chipColor[2] *= 0.35f;
		}

		// keycap: the coloured square is the item icon, the letter on it is
		// the key that buys it, so the panel is readable without a manual
		CG_FillRect( x + 6.0f, textY - 1.0f, keySize, keySize, chipColor );
		CG_DrawRect( x + 6.0f, textY - 1.0f, keySize, keySize, 1.0f, colorBorder );
		CG_Text_Paint( x + 6.0f + ( keySize - keyW ) * 0.5f, textY, 0.7f, colorCap,
			item->key, 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		CG_Text_Paint( x + 6.0f + keySize + 5.0f, textY, 0.7f, rowColor, label, 0, 0,
			ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		CG_Text_Paint( priceX - priceW, textY, 0.7f,
			owned ? colorPriceOwned : ( poor ? colorPricePoor : colorPrice ), price,
			0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );

		if ( owned )
		{
			const char *tag = "bought";
			float tagW = (float)CG_Text_Width( tag, 0.7f, FONT_SMALL );

			CG_Text_Paint( tagX - tagW, textY, 0.7f, colorBought, tag, 0, 0,
				ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
		}

		textY += rowHeight;
	}

	CG_Text_Paint( x + 8.0f, textY, 0.75f,
		( cgMoba.gold > 0 ) ? colorGold : colorUrgent,
		va( "Gold: %i", cgMoba.gold ), 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_MEDIUM );

	CG_Text_Paint( x + 8.0f, textY + rowHeight, 0.7f, colorHint,
		"buy G H J N X ;    B close", 0, 0, ITEM_TEXTSTYLE_SHADOWEDMORE, FONT_SMALL );
}
