/*
===========================================================================
MagicWands MOBA mod - OpenJK cgame (shop, hero select and ability bar)
===========================================================================
The server pushes three commands to the client, all of them only when the
state they carry actually changed:
  "mobaShop phase seconds gold itemMask level"          for the shop window
  "mobaDraft banned red blue taken action canAct seconds myHero myTeam step
   steps"                                              for the hero window
  "mobaAbilities heroId cd0 cd1 cd2 cd3 lv0 lv1 lv2 lv3" for the ability bar
Everything the client draws therefore stays in sync with the authoritative
game state without any guessing.

Keys: the shop window opens and closes on B, the four abilities of the hero are
on Q, E, C and V (see moba.cfg, the player may rebind them). Both work the same
way: a bind writes a one character token into a cvar, the cgame reads that cvar
every frame and sends the chat command the player would have typed. A JKA client
turns letter keys into buttons before the cgame ever sees them, so a bind is the
only way to get a key into the cgame.

Mouse: the shop window is a window like the hero one, it holds the mouse while
it is open. Click a tab (Defence, Attack, Consumables), click an item to buy it,
the arrow keys and Enter work as well. ESC closes the shop, it never reaches the
cgame as a key, the window reads it out of the mouse catcher the client took
away. The icons at the bottom of the screen can be clicked with the left mouse
button too, and that click is consumed so it does not also swing the saber.
=========================================================================
*/
#ifndef CG_MOBA_H
#define CG_MOBA_H
// Called from the server command table in cg_servercmds.c.
void		CG_Moba_ServerCommand_f( void );
void		CG_Moba_DraftCommand_f( void );
void		CG_Moba_AbilitiesCommand_f( void );
void		CG_Moba_DraftOpen_f( void );		// "mobaDraftOpen", server side !draft
// Called from CG_Draw2D, handles the shop keys, the ability keys and draws both
// windows plus the ability bar.
void		CG_Moba_Draw( void );
// Called from CG_KeyEvent in cg_newDraw.c before it decides what a key means.
// Returns qtrue when the hero window, the shop window or the ability bar used
// the key, so the game never sees it.
qboolean	CG_Moba_KeyEvent( int key, qboolean down );

#endif // CG_MOBA_H
