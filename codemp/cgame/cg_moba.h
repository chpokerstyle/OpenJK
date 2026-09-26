/*
===========================================================================
MagicWands MOBA mod - OpenJK cgame (on-screen shop)
===========================================================================
The server pushes "mobaShop phase seconds gold itemMask level" to the client
once per change. That is the only thing this file needs to know, so the panel
stays in sync with the authoritative game state without any guessing.
===========================================================================
*/

#ifndef CG_MOBA_H
#define CG_MOBA_H

// Called from the server command table in cg_servercmds.c.
void		CG_Moba_ServerCommand_f( void );
// Called from CG_Draw2D, handles the shop keys and draws the shop panel.
void		CG_Moba_Draw( void );

#endif // CG_MOBA_H
