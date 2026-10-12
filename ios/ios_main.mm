// iOS app shell: a UIKit application hosting a CAMetalLayer view driven by a
// CADisplayLink. No Storyboard, no scene manifest — a classic AppDelegate
// window. The display link runs the shared game loop (app_frame); touches and
// gesture recognizers feed the platform layer (plat_ios) that the game polls.
#import <UIKit/UIKit.h>

#import "app.h"
#import "or_types.h"
#import "gfx_metal.h"
#import "plat_ios.h"
#import "softkbd.h"

// The Metal view doubles as the keyboard's text target (UIKeyInput): while a
// text-entry screen is up it becomes first responder, which raises the system
// keyboard, and what the player types is queued for the game (softkbd.h).
@interface MetalView : UIView <UIKeyInput>
@property (nonatomic) UIKeyboardType keyboardType;
@property (nonatomic) UITextAutocapitalizationType autocapitalizationType;
@property (nonatomic) UITextAutocorrectionType autocorrectionType;
@property (nonatomic) UITextSpellCheckingType spellCheckingType;
@property (nonatomic) UITextSmartQuotesType smartQuotesType;
@property (nonatomic) UITextSmartDashesType smartDashesType;
@property (nonatomic) UITextSmartInsertDeleteType smartInsertDeleteType;
@property (nonatomic) UIReturnKeyType returnKeyType;
@property (nonatomic) BOOL wantsKeyboard;
@property (nonatomic) BOOL started;
@property (nonatomic) NSInteger originX; // safe-area left inset, pixels
@property (nonatomic) NSInteger originY; // safe-area top inset, pixels
@end

// The one game view, for the keyboard call the C side makes (main thread).
static __weak MetalView* s_view;

@implementation MetalView

+ (Class)layerClass { return [CAMetalLayer class]; }

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        // Names and room codes: capitals and digits, nothing to correct.
        _keyboardType           = UIKeyboardTypeASCIICapable;
        _autocapitalizationType = UITextAutocapitalizationTypeAllCharacters;
        _autocorrectionType     = UITextAutocorrectionTypeNo;
        _spellCheckingType      = UITextSpellCheckingTypeNo;
        _smartQuotesType        = UITextSmartQuotesTypeNo;
        _smartDashesType        = UITextSmartDashesTypeNo;
        _smartInsertDeleteType  = UITextSmartInsertDeleteTypeNo;
        _returnKeyType          = UIReturnKeyDone;
        s_view = self;
    }
    return self;
}

// --- Keyboard (UIKeyInput) --------------------------------------------------
// Only a first responder while the game wants the keyboard, so a stray
// becomeFirstResponder (e.g. on returning to the foreground) can't raise it.
- (BOOL)canBecomeFirstResponder { return self.wantsKeyboard; }
- (BOOL)hasText { return YES; } // so Delete always reaches us
- (void)insertText:(NSString*)text {
    [text enumerateSubstringsInRange:NSMakeRange(0, text.length)
                             options:NSStringEnumerationByComposedCharacterSequences
                          usingBlock:^(NSString* ch, NSRange r, NSRange er, BOOL* stop) {
        (void)r; (void)er; (void)stop;
        unichar u = [ch characterAtIndex:0];
        softkbd_push(u == '\n' || u == '\r' ? SOFTKBD_DONE
                     : (ch.length == 1 ? (int)u : '?'));
    }];
}
- (void)deleteBackward { softkbd_push(SOFTKBD_BACKSPACE); }

- (void)setKeyboardVisible:(BOOL)visible {
    self.wantsKeyboard = visible;
    if (visible) {
        // Cycling first responder brings back a keyboard the player hid (the
        // iPad's hide-keyboard key).
        if (self.isFirstResponder) [self resignFirstResponder];
        [self becomeFirstResponder];
    } else if (self.isFirstResponder) {
        [self resignFirstResponder];
    }
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (!self.window || self.started) return;
    self.started = YES;

    self.multipleTouchEnabled = YES;
    gfx_metal_attach((CAMetalLayer*)self.layer);
    [self updateDrawableSize];
    app_init();

    // Gesture recognizers (don't swallow the raw touches the game also reads).
    UITapGestureRecognizer* tap =
        [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(onTap:)];
    tap.cancelsTouchesInView = NO;
    [self addGestureRecognizer:tap];
    UISwipeGestureRecognizer* up =
        [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(onSwipeUp:)];
    up.direction = UISwipeGestureRecognizerDirectionUp;
    up.cancelsTouchesInView = NO;
    [self addGestureRecognizer:up];
    UISwipeGestureRecognizer* down =
        [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(onSwipeDown:)];
    down.direction = UISwipeGestureRecognizerDirectionDown;
    down.cancelsTouchesInView = NO;
    [self addGestureRecognizer:down];
    UISwipeGestureRecognizer* left =
        [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(onSwipeLeft:)];
    left.direction = UISwipeGestureRecognizerDirectionLeft;
    left.cancelsTouchesInView = NO;
    [self addGestureRecognizer:left];
    UISwipeGestureRecognizer* right =
        [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(onSwipeRight:)];
    right.direction = UISwipeGestureRecognizerDirectionRight;
    right.cancelsTouchesInView = NO;
    [self addGestureRecognizer:right];

    CADisplayLink* link = [CADisplayLink displayLinkWithTarget:self selector:@selector(onFrame:)];
    link.preferredFramesPerSecond = 60;
    [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)layoutSubviews { [super layoutSubviews]; [self updateDrawableSize]; }
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self updateDrawableSize]; }

- (void)updateDrawableSize {
    CAMetalLayer* layer = (CAMetalLayer*)self.layer;
    CGFloat scale = self.window ? self.window.screen.scale : [UIScreen mainScreen].scale;
    self.contentScaleFactor = scale;
    layer.contentsScale = scale;
    CGSize full = CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
    if (full.width  < 1) full.width  = 1;
    if (full.height < 1) full.height = 1;
    layer.drawableSize = full;

    // Draw inside the safe area: the game's (0,0)..(w,h) maps to the region
    // between the notch/Dynamic Island and the home indicator. The area outside
    // clears to the frame's gfx_clear colour, so it's seamless.
    UIEdgeInsets ins = self.safeAreaInsets;
    NSInteger ox = (NSInteger)(ins.left * scale);
    NSInteger oy = (NSInteger)(ins.top  * scale);
    NSInteger sw = (NSInteger)full.width  - ox - (NSInteger)(ins.right  * scale);
    NSInteger sh = (NSInteger)full.height - oy - (NSInteger)(ins.bottom * scale);
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    self.originX = ox;
    self.originY = oy;
    gfx_metal_set_viewport((int)full.width, (int)full.height, (int)ox, (int)oy);
    plat_ios_set_screen((int)sw, (int)sh);
}

- (void)onFrame:(CADisplayLink*)link { (void)link; app_frame(); }

// --- Touches: publish the set of active points (in drawable pixels). --------
- (void)publishTouches:(UIEvent*)event {
    NSSet<UITouch*>* all = [event allTouches];
    Vector2 pts[16];
    int n = 0;
    CGFloat scale = self.contentScaleFactor;
    for (UITouch* t in all) {
        if (t.phase == UITouchPhaseEnded || t.phase == UITouchPhaseCancelled) continue;
        if (n >= 16) break;
        CGPoint p = [t locationInView:self];
        // Into the game's safe-area coordinate space (matches what's drawn).
        pts[n].x = (float)(p.x * scale - self.originX);
        pts[n].y = (float)(p.y * scale - self.originY);
        n++;
    }
    plat_ios_set_touches(pts, n);
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)touches; [self publishTouches:event]; }
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)touches; [self publishTouches:event]; }
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)touches; [self publishTouches:event]; }
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)touches; [self publishTouches:event]; }

- (void)onTap:(UITapGestureRecognizer*)g       { (void)g; plat_ios_post_gesture(GESTURE_TAP); }
- (void)onSwipeUp:(UISwipeGestureRecognizer*)g  { (void)g; plat_ios_post_gesture(GESTURE_SWIPE_UP); }
- (void)onSwipeDown:(UISwipeGestureRecognizer*)g{ (void)g; plat_ios_post_gesture(GESTURE_SWIPE_DOWN); }
- (void)onSwipeLeft:(UISwipeGestureRecognizer*)g  { (void)g; plat_ios_post_gesture(GESTURE_SWIPE_LEFT); }
- (void)onSwipeRight:(UISwipeGestureRecognizer*)g { (void)g; plat_ios_post_gesture(GESTURE_SWIPE_RIGHT); }

@end

extern "C" void plat_ios_set_keyboard(bool visible) {
    [s_view setKeyboardVisible:visible ? YES : NO];
}

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow* window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication*)application
        didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    (void)application; (void)launchOptions;
    CGRect bounds = [UIScreen mainScreen].bounds;
    self.window = [[UIWindow alloc] initWithFrame:bounds];
    UIViewController* vc = [[UIViewController alloc] init];
    vc.view = [[MetalView alloc] initWithFrame:bounds];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];

    [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationDidBecomeActiveNotification
        object:nil queue:nil usingBlock:^(NSNotification* n){ (void)n; plat_ios_set_focus(true); }];
    [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationWillResignActiveNotification
        object:nil queue:nil usingBlock:^(NSNotification* n){ (void)n; plat_ios_set_focus(false); }];
    return YES;
}

@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
