#ifndef OPENRACKEM_SHARE_H
#define OPENRACKEM_SHARE_H

// Hand a line of text (a private room's invite) to the platform: the system
// share sheet on iOS and Android (and phone browsers that have one), the
// clipboard on desktop and desktop browsers. The caller shows a "Copied"
// notice when the text went to the clipboard.
typedef enum {
    SHARE_NONE = 0,   // nothing happened (no way to share on this build)
    SHARE_SHEET,      // the system share sheet is up
    SHARE_COPIED,     // the text is on the clipboard
} ShareResult;

ShareResult share_text(const char* text);

#endif // OPENRACKEM_SHARE_H
