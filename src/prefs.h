#ifndef OPENRACKEM_PREFS_H
#define OPENRACKEM_PREFS_H

// Tiny persisted settings kept across launches on every platform: the player
// name and the seat key of a live online match (Return to Game). Backed by a
// per-user config file on desktop, the app's private storage on Android/iOS,
// and localStorage on the web. All access is in-memory; call prefs_save() to
// persist a change.

// Load persisted prefs into memory. Call once at startup, after the window /
// storage context exists. A no-op if nothing is stored yet.
void prefs_load(void);

// The current player name ("" when unset). Never NULL.
const char* prefs_name(void);

// Replace the in-memory name (sanitized to printable ASCII, length-capped).
// Does not persist until prefs_save().
void prefs_set_name(const char* name);

// The member key of the seat we hold in a live match ("" = none), so a
// relaunch can offer Return to Game.
const char* prefs_seat_key(void);
void prefs_set_seat_key(const char* key);

// Persist the current in-memory prefs to storage. Best-effort (silent on I/O
// failure — a missing value just isn't remembered).
void prefs_save(void);

#endif // OPENRACKEM_PREFS_H
