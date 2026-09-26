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

// mirrors mobaPhase_t from g_moba.h: LOBBY 0, DRAFT 1, BUY 2, FIGHT 3, ROUNDEND 4
#define CG_MOBA_PHASE_BUY	2

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
// proportional and would break a fixed column layout.
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
