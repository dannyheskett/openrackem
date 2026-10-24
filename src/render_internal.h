#ifndef OPENRACKEM_RENDER_INTERNAL_H
#define OPENRACKEM_RENDER_INTERNAL_H

// Private interface shared between the renderer translation units:
//   render.c           — common state, shared draw helpers, lifecycle
//   render_portrait.c  — the table renderer (full screen on phones, a centred
//                        column on desktop)

#include "render.h"
#include "gfx.h"
#include <stddef.h> // size_t

// Table palette (defined in render.c), shared by both renderers.
extern const Color TABLE_BG;      // felt background
extern const Color CARD_FACE;     // face-up card fill
extern const Color CARD_BACK;     // face-down card fill
extern const Color CARD_TEXT;     // card number
extern const Color CARD_EDGE;     // card outline
extern const Color SLOT_LABEL;    // the 5..50 rack labels
extern const Color ACCENT;        // cursor / current-turn highlight
extern const Color TURN_INK;      // type drawn on the accent-filled turn bar

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

// Whether `seat` is the one acting right now: drawing or placing. False while
// dealing and on the round / match screens, when nobody's turn is running.
bool seat_is_active(const Game* g, int seat);

// Whose turn it is, for the status line when it isn't the human's: "CPU 2'S
// TURN", or an online player's name. Written into `buf`, which is returned.
const char* turn_owner_text(const Game* g, char* buf, size_t n);

// Deal-animation progress: how many of `seat`'s cards have visibly arrived
// (they land in slot #50 first, downward), and whether the first discard has
// been flipped yet. Purely presentation — the full deal exists in the state.
int  deal_cards_for_seat(const Game* g, int seat);
bool deal_discard_flipped(const Game* g);

// Card-flight tween: true while a card is visibly travelling; progress runs
// 0 -> 1 with smoothstep easing. Each renderer maps the anim's kind/seat/slot
// to its own source and destination rectangles and draws the flyer on top.
bool  anim_in_flight(const Game* g);
float anim_progress(const Game* g);
// Interpolate a card between two rects at progress t and draw it.
void draw_flying_card(Rectangle from, Rectangle to, float t, int value, bool face_up);

// Table hit zones for touch and mouse (captured per frame; read by
// render_table_hit_test).
void table_hits_reset(void);
void table_hit_set(int id, Rectangle r);   // id: 0..9 slots, HIT_STOCK, HIT_DISCARD

// Hit capture is on by default; the renderer turns it off for the recorder's
// fixed-size pass so only the window's geometry is ever hit-tested. The offset
// maps rectangles drawn inside the desktop column into window pixels.
void hit_capture_enable(bool on);
void hit_offset_set(float ox, float oy);

// Rounded rectangle with the corner radius in pixels (clamped to half the
// shorter side), over gfx_rect_rounded's roundness fraction.
void rect_rounded_px(int x, int y, int w, int h, int radius, Color color);

// Computed slot-picker geometry + its shared drawer (defined in render.c),
// filled by each renderer from its own sizing. `capture` records the
// slot and button rectangles as tap / click targets (window pass only).
typedef struct {
    int cx, px, py, panel_w, panel_h;
    int title_size, title_y;
    int slots_y, slot_w, slot_h, slot_gap, slot_fs;
    int band_h;                  // the spinner bands above and below the slots
    int hint_y, hint_fs;
    int btn_y, btn_w, btn_h, btn_fs;
} PickerLayout;
void draw_picker_panel(PickerLayout p, const char* title, const char* slots,
                       int cursor, const char* alphabet, const char* hint,
                       const char* ok_label, bool capture);

// The table renderer's entry points (render_portrait.c), called by render.c.
void render_frame_portrait(const Game* game, const TableUi* ui);
void render_pause_portrait(const Game* game, const TableUi* ui);
void render_picker_portrait(const char* title, const char* slots, int cursor,
                            const char* alphabet, const char* hint,
                            const char* ok_label);

#endif // OPENRACKEM_RENDER_INTERNAL_H
