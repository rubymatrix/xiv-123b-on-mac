/* The Mac app opened from Finder (tests/xivboot.c calls mac_launch first): the game folder, and the
 * sign-in window when nothing gave the launch argument.
 *
 * The game folder: the app's defaults (xiv.game, which the installer writes), else chosen once with
 * a folder panel and kept. The sign-in: username, password, Remember password and Sign In in a
 * window; the server (and the lobby, when it is another host) in a dialog behind the gear (opened by
 * itself on the first run). It posts as the iOS screen does (signin_core.m) and sets XIV_ARGS, XIV_TICK, XIV_SERVER_HOST
 * and XIV_LOBBY for the game. Started with arguments (tools/xiv_login.py, xivboot <exe> ...), nothing
 * here happens. */
#import <AppKit/AppKit.h>
#include <stdlib.h>
#include <string.h>

#import "signin_core.h"
#include "xiv_args.h"

static NSString* game_folder(void)
{
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
    NSString* game = [d stringForKey:@"xiv.game"];
    NSFileManager* fm = NSFileManager.defaultManager;
    if (game.length && [fm fileExistsAtPath:[game stringByAppendingPathComponent:@"ffxivgame.exe"]])
        return game;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.message = @"Choose your FINAL FANTASY XIV folder (patched to 1.23b).";
    panel.prompt = @"Use This Folder";
    while ([panel runModal] == NSModalResponseOK)
    {
        NSString* path = panel.URL.path;
        if ([fm fileExistsAtPath:[path stringByAppendingPathComponent:@"ffxivgame.exe"]])
        {
            [d setObject:path forKey:@"xiv.game"];
            return path;
        }
        panel.message = @"That folder has no ffxivgame.exe. Choose the FINAL FANTASY XIV folder itself.";
    }
    return nil;
}

/* --- the window ------------------------------------------------------------------------------------ */
@interface XivMacSignIn : NSObject <NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSTextField* user;
@property(nonatomic, strong) NSSecureTextField* password;
@property(nonatomic, strong) NSButton *remember, *go;
@property(nonatomic, strong) NSTextField* status;
@property(nonatomic, strong) NSProgressIndicator* spinner;
@property(nonatomic, copy) NSString* session;
@property(nonatomic) BOOL quit;
@end

@implementation XivMacSignIn

- (instancetype)init
{
    if (!(self = [super init]))
        return nil;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 380, 220)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"FINAL FANTASY XIV 1.23b";
    self.window.delegate = self;

    NSTextField* title = [NSTextField labelWithString:@"Sign in"];
    title.font = [NSFont boldSystemFontOfSize:17];
    NSButton* gear = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:@"Server"]
                                        target:self
                                        action:@selector(showServer)];
    gear.bordered = NO;
    gear.toolTip = @"Server";
    NSStackView* top = [NSStackView stackViewWithViews:@[ title, [NSView new], gear ]];

    self.user = [NSTextField textFieldWithString:signin_saved(@"xiv.user")];
    self.user.placeholderString = @"Username";
    self.password = [NSSecureTextField new];
    self.password.placeholderString = @"Password";
    self.password.target = self;
    self.password.action = @selector(signIn);
    self.remember = [NSButton checkboxWithTitle:@"Remember password" target:nil action:nil];
    self.remember.state = signin_remember() ? NSControlStateValueOn : NSControlStateValueOff;
    NSString* page = signin_login_page(signin_saved(@"xiv.server"));
    if (signin_remember() && self.user.stringValue.length && page)
        self.password.stringValue = signin_key_load(page, self.user.stringValue) ?: @"";
    self.spinner = [NSProgressIndicator new];
    self.spinner.style = NSProgressIndicatorStyleSpinning;
    self.spinner.controlSize = NSControlSizeSmall;
    self.spinner.displayedWhenStopped = NO;
    self.go = [NSButton buttonWithTitle:@"Sign In" target:self action:@selector(signIn)];
    self.go.keyEquivalent = @"\r";
    NSStackView* row = [NSStackView stackViewWithViews:@[ self.remember, [NSView new], self.spinner, self.go ]];
    self.status = [NSTextField wrappingLabelWithString:@""];
    self.status.textColor = NSColor.systemRedColor;
    self.status.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];

    NSStackView* form = [NSStackView stackViewWithViews:@[ top, self.user, self.password, row, self.status ]];
    form.orientation = NSUserInterfaceLayoutOrientationVertical;
    form.alignment = NSLayoutAttributeLeading;
    form.spacing = 10;
    form.edgeInsets = NSEdgeInsetsMake(18, 20, 16, 20);
    for (NSView* v in @[ top, self.user, self.password, row, self.status ])
        [v.widthAnchor constraintEqualToAnchor:form.widthAnchor constant:-40].active = YES;
    self.window.contentView = form;
    [self.window center];
    return self;
}

- (void)busy:(BOOL)busy status:(NSString*)text error:(BOOL)error
{
    self.go.enabled = !busy;
    busy ? [self.spinner startAnimation:nil] : [self.spinner stopAnimation:nil];
    self.status.stringValue = text ?: @"";
    self.status.textColor = error ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
}

/* the server, and the lobby when it is another host, in a dialog of their own */
- (void)showServer
{
    NSAlert* a = [NSAlert new];
    a.messageText = @"Server";
    a.informativeText = @"The server's address (host, or host:port), from whoever runs it. The lobby is the same host unless they gave you another.";
    NSTextField* server = [NSTextField textFieldWithString:signin_saved(@"xiv.server")];
    server.placeholderString = @"Server address, e.g. 203.0.113.5";
    NSTextField* lobby = [NSTextField textFieldWithString:signin_saved(@"xiv.lobby")];
    lobby.placeholderString = @"Lobby, if another host (optional)";
    NSStackView* fields = [NSStackView stackViewWithViews:@[ server, lobby ]];
    fields.orientation = NSUserInterfaceLayoutOrientationVertical;
    fields.frame = NSMakeRect(0, 0, 300, 56);
    [server.widthAnchor constraintEqualToConstant:300].active = YES;
    [lobby.widthAnchor constraintEqualToConstant:300].active = YES;
    a.accessoryView = fields;
    [a addButtonWithTitle:@"Save"];
    [a addButtonWithTitle:@"Cancel"];
    a.window.initialFirstResponder = server;
    if ([a runModal] != NSAlertFirstButtonReturn)
        return;
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
    NSCharacterSet* ws = NSCharacterSet.whitespaceCharacterSet;
    [d setObject:[server.stringValue stringByTrimmingCharactersInSet:ws] forKey:@"xiv.server"];
    [d setObject:[lobby.stringValue stringByTrimmingCharactersInSet:ws] forKey:@"xiv.lobby"];
    NSString* page = signin_login_page(signin_saved(@"xiv.server"));
    if (!self.password.stringValue.length && signin_remember() && self.user.stringValue.length && page)
        self.password.stringValue = signin_key_load(page, self.user.stringValue) ?: @"";
}

- (void)signIn
{
    NSString* user = [self.user.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSString* password = self.password.stringValue;
    if (!signin_saved(@"xiv.server").length)
    {
        [self showServer];
        return;
    }
    if (!user.length || !password.length)
    {
        [self busy:NO status:@"Enter your username and password." error:YES];
        return;
    }
    [self busy:YES status:@"Signing in…" error:NO];
    BOOL remember = self.remember.state == NSControlStateValueOn;
    signin_post(signin_saved(@"xiv.server"), user, password, ^(NSString* sid, NSString* why) {
        if (!sid)
        {
            [self busy:NO status:why error:YES];
            return;
        }
        signin_keep(user, password, remember);
        self.session = sid;
        [NSApp stopModal];
    });
}

- (void)windowWillClose:(NSNotification*)n
{
    (void)n;
    if (!self.session)
        self.quit = YES, [NSApp stopModal];
}

@end

/* Called first in xivboot's main on the Mac. Opened with no arguments (from Finder): the game folder
 * becomes the arguments and, unless XIV_ARGS is set, the sign-in window gives the launch argument.
 * Returns 0 when the player closed the window or chose no folder: the app quits. */
int mac_launch(int* argc, char*** argv)
{
    if (*argc >= 2)
        return 1; /* started with an exe (xiv_login.py, a terminal): as before */
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activate];
        NSString* game = game_folder();
        if (!game)
            return 0;
        static char* args[4];
        args[0] = (*argv)[0];
        args[1] = strdup([game stringByAppendingPathComponent:@"ffxivgame.exe"].fileSystemRepresentation);
        args[2] = strdup(game.fileSystemRepresentation);
        args[3] = NULL;
        *argc = 3, *argv = args;
        if (getenv("XIV_ARGS"))
            return 1;
        XivMacSignIn* s = [XivMacSignIn new];
        [s.window makeKeyAndOrderFront:nil];
        if (!signin_saved(@"xiv.server").length)
            dispatch_async(dispatch_get_main_queue(), ^{ [s showServer]; });
        [NSApp runModalForWindow:s.window];
        if (!s.session)
            return 0;
        [s.window orderOut:nil];
        char arg[512];
        if (!xiv_launch_arg(s.session.UTF8String, XIV_ARGS_TICK, arg, sizeof arg))
            return 0;
        setenv("XIV_ARGS", arg, 1);
        setenv("XIV_TICK", "0x10000", 1);
        setenv("XIV_SERVER_HOST", signin_lobby().UTF8String, 0); /* the game connects there */
        setenv("XIV_LOBBY", signin_lobby().UTF8String, 0);
        return 1;
    }
}
