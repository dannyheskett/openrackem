#ifndef OPENRACKEM_SOUND_H
#define OPENRACKEM_SOUND_H

#include <stdbool.h>

// Procedural retro sound effects. Every effect is synthesized at startup from
// simple square / swept / noise waveforms (no audio files), giving the crunchy
// chiptune feel of classic 8-bit hardware. Sound is disabled by default.

typedef enum {
    SFX_DRAW = 0,    // a card drawn from the stock or discard pile
    SFX_PLACE,       // held card exchanged into a rack slot
    SFX_DISCARD,     // held card thrown onto the discard pile
    SFX_TURN,        // the next player's turn began
    SFX_INVALID,     // an illegal move was attempted
    SFX_ROUND_WIN,   // the human went out (RACK 'EM!)
    SFX_ROUND_LOSE,  // someone else went out (or a stalemate)
    SFX_BONUS,       // bonus-variant run points awarded
    SFX_MATCH_WIN,   // the match was decided
    SFX_MENU_MOVE,   // menu cursor moved
    SFX_MENU_SELECT, // menu item chosen
    SFX_PAUSE,       // game paused
    SFX_COUNT
} SfxId;

void sound_init(void);       // open the audio device and synthesize all effects
void sound_shutdown(void);   // free effects and close the audio device
bool sound_is_enabled(void);
void sound_toggle(void);
void sound_play(SfxId id);   // no-op when sound is disabled

#endif // OPENRACKEM_SOUND_H
