#ifndef OPENRACKEM_RENDER_INTERNAL_H
#define OPENRACKEM_RENDER_INTERNAL_H

// Private interface shared between the renderer translation units:
//   render.c           — common state, shared draw helpers, lifecycle, dispatch
//   render_portrait.c  — the portrait (touch) renderer    (body under OR_PORTRAIT)
//   render_landscape.c — the landscape (desktop) renderer (body under OR_LANDSCAPE)
// render_portrait/landscape.c compile to empty objects on platforms that don't
// use them, so all three can sit in the build's source list unconditionally.

#include "render.h"
#include "gfx.h"

// Table palette (defined in render.c), shared by both renderers.
extern const Color TABLE_BG;      // felt background
extern const Color CARD_FACE;     // face-up card fill
extern const Color CARD_BACK;     // face-down card fill
extern const Color CARD_TEXT;     // card number
extern const Color CARD_EDGE;     // card outline
extern const Color SLOT_LABEL;    // the 5..50 rack labels
extern const Color ACCENT;        // cursor / current-turn highlight

// Shared draw helpers (defined in render.c).
// A card at (x, y, w, h): face up shows its number, face down shows the back
// pattern. `highlight` outlines it in the accent colour (cursor / selection).
void draw_card(int x, int y, int w, int h, int value, bool face_up, bool highlight);
// An empty card outline (the discard pile before any card, a vacated slot).
void draw_card_outline(int x, int y, int w, int h);
void draw_center_panel_at(int w, int h, int panel_w, int panel_h, int ts, int ss,
                          int title_dy, int sub_dy, const char* title,
                          const char* subtitle, Color title_color);

// A compact rack strip: ten cards of (cw x ch) laid horizontally with a 1px
// gap, face up or as backs (opponents). Used by both renderers for the
// opponent panels and the round-over reveal.
void draw_mini_rack(int x, int y, int cw, int ch, const Rack* rack, bool face_up);

// Seat display name: "YOU" for the human seat, "CPU n" otherwise. Returns a
// static buffer valid until the next call for that seat.
const char* seat_name(const Game* g, int seat);

// Deal-animation progress: how many of `seat`'s cards have visibly arrived
// (they land in slot #50 first, downward), and whether the first discard has
// been flipped yet. Purely presentation — the full deal exists in the state.
int  deal_cards_for_seat(const Game* g, int seat);
bool deal_discard_flipped(const Game* g);

// Table hit zones for touch (captured per frame; read by render_table_hit_test).
void table_hits_reset(void);
void table_hit_set(int id, Rectangle r);   // id: 0..9 slots, HIT_STOCK, HIT_DISCARD

// Computed menu geometry + the shared menu drawer (defined in render.c). Each
// renderer fills the layout from its own sizing.
typedef struct {
    int cx, px, py, panel_w, panel_h;
    int title_size, title_y, items_y, line_h, item_fs;
} MenuLayout;
void draw_menu_panel(MenuLayout m, const char* title, const char* const* items,
                     int count, int selected, int gap_before, bool capture);

// Per-renderer entry points (defined in render_portrait/landscape.c), called by
// the OR_DISPATCH macro in render.c.
#ifdef OR_PORTRAIT
void render_frame_portrait(const Game* game, const TableUi* ui);
void render_pause_portrait(const Game* game, const TableUi* ui);
void render_menu_portrait(const char* title, const char* const* items, int count,
                          int selected, int gap_before);
#endif
#ifdef OR_LANDSCAPE
extern RenderTexture2D canvas; // created in render_init, blitted by present()
void render_frame_landscape(const Game* game, const TableUi* ui);
void render_pause_landscape(const Game* game, const TableUi* ui);
void render_menu_landscape(const char* title, const char* const* items, int count,
                           int selected, int gap_before);
#endif

#endif // OPENRACKEM_RENDER_INTERNAL_H
