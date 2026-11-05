#ifndef OPENRACKEM_PREFS_H
#define OPENRACKEM_PREFS_H

#include <stdbool.h>

// Tiny persisted settings kept across launches on every platform: the player
// name, the seat key of a live online match (Return to Game), and the settings
// the last online room was made with (New Room opens as it was left). Backed by a
// per-user config file on desktop, the app's private storage on Android/iOS,
// and localStorage on the web. All access is in-memory; call prefs_save() to
// persist a change.

// Load persisted prefs into memory. Call once at startup, after the window /
// storage context exists. A no-op if nothing is stored yet.
void prefs_load(void);

// The current player name ("" when unset). Never NULL.
const char* prefs_name(void);

// Replace the in-memory name (sanitized to printable ASCII without quotes or
// backslashes, length-capped).
// Does not persist until prefs_save().
void prefs_set_name(const char* name);

// The member key of the seat we hold in a live match ("" = none), so a
// relaunch can offer Return to Game.
const char* prefs_seat_key(void);
void prefs_set_seat_key(const char* key);

// The settings the New Room screen was last left with: seats, play-to, and
// the Bonus and Partners variants. prefs_room() returns false when nothing is
// stored yet. The values are whatever the file holds, so the caller brings
// them into range before using them (netgame_room_normalize).
bool prefs_room(int* seats, int* target, bool* bonus, bool* partners);
void prefs_set_room(int seats, int target, bool bonus, bool partners);

// Persist the current in-memory prefs to storage. Best-effort (silent on I/O
// failure — a missing value just isn't remembered).
void prefs_save(void);

#endif // OPENRACKEM_PREFS_H
