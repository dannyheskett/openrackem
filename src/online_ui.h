#ifndef OPENRACKEM_ONLINE_UI_H
#define OPENRACKEM_ONLINE_UI_H

#include "netgame.h"
#include "platform.h"
#include "or_types.h"
#include <stdbool.h>

// The online screens drawn in the table's portrait column: the room list
// (Play Online) and the waiting room. Pure presentation: main.c owns the
// session and the input, and maps taps back through online_hit_test().

// Rows of the room list, top to bottom. The first few are pinned (they never
// scroll); the rooms follow and scroll beneath them.
typedef enum {
    OLR_RETURN = 0,   // Return to Game (a live seat is held)
    OLR_NEW,          // New Room
    OLR_ROOM,         // a room (index into NetGame.rooms)
} OlRowKind;

typedef struct {
    OlRowKind kind;
    int       room;   // OLR_ROOM: index into ng->rooms
} OlRow;

#define OL_MAX_ROWS (2 + NG_LIST_MAX)

// Waiting-room buttons (focus order follows online_lobby_buttons()).
enum { OLB_READY = 0, OLB_FORCE, OLB_SWITCH, OLB_LEAVE };

// What a tap landed on.
enum {
    OL_HIT_NONE    = -1,
    OL_HIT_ROW     = 0,     // + row index (room list)
    OL_HIT_BACK    = 102,
    OL_HIT_BUTTON  = 200,   // + OLB_* (waiting room)
};

typedef struct {
    const NetGame* ng;
    const char*    player;       // our name
    bool           has_return;   // a live seat is held: Return to Game
    int            sel;          // list: selected row; lobby: focused button
    int            scroll;       // list: first room row shown (updated by render)
    const char*    notice;       // a short line to show, or NULL
} OnlineView;

// Build the room list's rows. Returns the count.
int online_list_rows(const OnlineView* v, OlRow rows[OL_MAX_ROWS]);
int online_list_pinned(const OlRow* rows, int n);   // how many are pinned

// The waiting room's buttons in focus order. Returns the count.
int online_lobby_buttons(const OnlineView* v, int ids[4]);

// The waiting room's result line for the last match ("" when there is none):
// who won and the final scores, from the session's held MATCH_OVER game.
void online_result_line(const NetGame* ng, char* out, size_t cap);

void online_render_list(OnlineView* v);
void online_render_lobby(const OnlineView* v);

int online_hit_test(Vector2 p);

#endif // OPENRACKEM_ONLINE_UI_H
