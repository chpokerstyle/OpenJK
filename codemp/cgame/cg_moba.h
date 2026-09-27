/*
===========================================================================
MagicWands MOBA mod - OpenJK cgame (shop and hero select)
===========================================================================
The server pushes two commands to the client, both once per change:
"mobaShop phase seconds gold itemMask level" for the buy panel and
"mobaDraft banned red blue action canAct seconds myHero step steps" for the
hero select window. Those are the only things this file needs to know, so
everything it draws stays in sync with the authoritative game state without
any guessing.
===========================================================================
*/

#ifndef CG_MOBA_H
#define CG_MOBA_H

// Called from the server command table in cg_servercmds.c.
void		CG_Moba_ServerCommand_f( void );
void		CG_Moba_DraftCommand_f( void );
// Called from CG_Draw2D, handles the shop keys and draws both panels.
void		CG_Moba_Draw( void );
// Called from CG_KeyEvent in cg_newDraw.c before it decides what a key means.
// Returns qtrue when the hero window used the key, so the game never sees it.
qboolean	CG_Moba_KeyEvent( int key, qboolean down );

#endif // CG_MOBA_H
