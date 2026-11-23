/* The sign-in screen on iOS (tests/xivboot_ios.c, before the game starts): the server, the account
 * and its password, in a UIKit form of its own window. Sign In posts the account to the 1.23b login
 * page as ffxivlogin.exe uses it (http://<server>/account/content/ffxivlogin, as Meteor-PatchLogin-Server
 * serves it), and the session id in the <x-sqexauth sid="..."> tag of its answer becomes the game's
 * launch argument (xiv_args.c). An answer without the tag is a refusal.
 *
 * The screen asks for the account only; the server (host, or host:port) and the lobby, when it is
 * another host than the server's, are in a sheet behind the gear (opened by itself on the first run).
 * Both, and the username, are kept in the app's defaults;
 * the password in the keychain while "Remember password" is on. Runs on the main thread: the window is shown and the run
 * loop turned until a sign-in succeeds; then the window goes and the game makes its own. */
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <UIKit/UIKit.h>
#include <string.h>

#include "xiv_args.h"

static NSString* const kService = @"FINAL FANTASY XIV 1.23b sign-in";

/* --- the password in the keychain: one per login page and username --------------------------- */
static NSDictionary* key_query(NSString* login, NSString* user)
{
    return @{ (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
              (__bridge id)kSecAttrService : kService,
              (__bridge id)kSecAttrAccount : [NSString stringWithFormat:@"%@ %@", user, login] };
}

static NSString* key_load(NSString* login, NSString* user)
{
    NSMutableDictionary* q = [key_query(login, user) mutableCopy];
    q[(__bridge id)kSecReturnData] = @YES;
    CFTypeRef data = NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &data) != errSecSuccess || !data)
        return nil;
    return [[NSString alloc] initWithData:(__bridge_transfer NSData*)data encoding:NSUTF8StringEncoding];
}

static void key_save(NSString* login, NSString* user, NSString* password)
{
    SecItemDelete((__bridge CFDictionaryRef)key_query(login, user));
    if (!password)
        return;
    NSMutableDictionary* q = [key_query(login, user) mutableCopy];
    q[(__bridge id)kSecValueData] = [password dataUsingEncoding:NSUTF8StringEncoding];
    q[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    SecItemAdd((__bridge CFDictionaryRef)q, NULL);
}

/* a form field's value: everything but letters, digits and -._~ percent-encoded (a password's & = +) */
static NSString* form_value(NSString* v)
{
    NSCharacterSet* keep = [NSCharacterSet characterSetWithCharactersInString:
        @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"];
    return [v stringByAddingPercentEncodingWithAllowedCharacters:keep];
}

/* --- shared field and label makers ------------------------------------------------------------ */
static UITextField* make_field(NSString* placeholder, BOOL secure, id<UITextFieldDelegate> delegate)
{
    UITextField* f = [UITextField new];
    f.placeholder = placeholder;
    f.borderStyle = UITextBorderStyleRoundedRect;
    f.autocorrectionType = UITextAutocorrectionTypeNo;
    f.autocapitalizationType = UITextAutocapitalizationTypeNone;
    f.secureTextEntry = secure;
    f.returnKeyType = UIReturnKeyNext;
    f.delegate = delegate;
    [f.heightAnchor constraintEqualToConstant:40].active = YES;
    return f;
}

static UILabel* make_label(NSString* text, UIFontTextStyle style, UIColor* color)
{
    UILabel* l = [UILabel new];
    l.text = text;
    l.font = [UIFont preferredFontForTextStyle:style];
    l.textColor = color;
    return l;
}

/* a stack in a scroll view that ends at the keyboard, centred, at most 420 points wide */
static void place_form(UIViewController* vc, UIStackView* form, CGFloat top)
{
    form.translatesAutoresizingMaskIntoConstraints = NO;
    UIScrollView* scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [vc.view addSubview:scroll];
    [scroll addSubview:form];
    UILayoutGuide* safe = vc.view.safeAreaLayoutGuide;
    UILayoutGuide* content = scroll.contentLayoutGuide;
    UILayoutGuide* frame = scroll.frameLayoutGuide;
    NSLayoutConstraint* wide = [form.widthAnchor constraintEqualToAnchor:frame.widthAnchor constant:-32];
    wide.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:vc.view.keyboardLayoutGuide.topAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [form.topAnchor constraintEqualToAnchor:content.topAnchor constant:top],
        [form.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-12],
        [form.centerXAnchor constraintEqualToAnchor:frame.centerXAnchor],
        [form.widthAnchor constraintLessThanOrEqualToConstant:420],
        wide,
    ]];
}

static NSString* saved(NSString* key)
{
    return [[NSUserDefaults.standardUserDefaults stringForKey:key]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] ?: @"";
}

/* the host a server address names: no scheme, no port, no path */
static NSString* host_of(NSString* server)
{
    NSString* s = server;
    NSRange r = [s rangeOfString:@"://"];
    if (r.location != NSNotFound)
        s = [s substringFromIndex:NSMaxRange(r)];
    NSRange slash = [s rangeOfString:@"/"];
    if (slash.location != NSNotFound)
        s = [s substringToIndex:slash.location];
    NSRange colon = [s rangeOfString:@":" options:NSBackwardsSearch];
    return colon.location != NSNotFound ? [s substringToIndex:colon.location] : s;
}

/* the login page on the server: http://<host[:port]>/account/content/ffxivlogin */
static NSString* login_page(NSString* server)
{
    NSString* s = server;
    NSRange r = [s rangeOfString:@"://"];
    NSString* scheme = r.location != NSNotFound ? [s substringToIndex:r.location] : @"http";
    if (r.location != NSNotFound)
        s = [s substringFromIndex:NSMaxRange(r)];
    NSRange slash = [s rangeOfString:@"/"];
    if (slash.location != NSNotFound)
        s = [s substringToIndex:slash.location];
    return s.length ? [NSString stringWithFormat:@"%@://%@/account/content/ffxivlogin", scheme, s] : nil;
}

/* the lobby the game connects to: the one set, else the server's host */
static NSString* lobby_host(void)
{
    NSString* lobby = saved(@"xiv.lobby");
    return lobby.length ? host_of(lobby) : host_of(saved(@"xiv.server"));
}

/* --- the server: a sheet behind the gear --------------------------------------------------------- */
@interface XivServerSheet : UIViewController <UITextFieldDelegate>
@property(nonatomic, strong) UITextField *server, *lobby;
@property(nonatomic, copy) void (^done)(void);
@end

@implementation XivServerSheet

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.title = @"Server";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                           target:self
                                                                                           action:@selector(finish)];
    self.server = make_field(@"Server address, e.g. 203.0.113.5 or 203.0.113.5:8000", NO, self);
    self.server.keyboardType = UIKeyboardTypeURL;
    self.server.text = saved(@"xiv.server");
    self.lobby = make_field(@"The server's host", NO, self);
    self.lobby.keyboardType = UIKeyboardTypeURL;
    self.lobby.returnKeyType = UIReturnKeyDone;
    self.lobby.text = saved(@"xiv.lobby");
    UIColor* dim = UIColor.secondaryLabelColor;
    UIStackView* form = [[UIStackView alloc] initWithArrangedSubviews:@[
        make_label(@"SERVER ADDRESS", UIFontTextStyleFootnote, dim), self.server,
        make_label(@"LOBBY, IF ANOTHER HOST", UIFontTextStyleFootnote, dim), self.lobby
    ]];
    form.axis = UILayoutConstraintAxisVertical;
    form.spacing = 6;
    [form setCustomSpacing:14 afterView:self.server];
    place_form(self, form, 16);
}

- (BOOL)textFieldShouldReturn:(UITextField*)f
{
    if (f == self.server)
        [self.lobby becomeFirstResponder];
    else
        [self finish];
    return NO;
}

- (void)finish
{
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
    [d setObject:[self.server.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] forKey:@"xiv.server"];
    [d setObject:[self.lobby.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] forKey:@"xiv.lobby"];
    [self dismissViewControllerAnimated:YES completion:self.done];
}

@end

/* --- the account: the screen itself ------------------------------------------------------------- */
@interface XivSignIn : UIViewController <UITextFieldDelegate>
@property(nonatomic, strong) UITextField *user, *password;
@property(nonatomic, strong) UISwitch* remember;
@property(nonatomic, strong) UIButton* go;
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UIActivityIndicatorView* spinner;
@property(nonatomic, copy) NSString* session; /* the 56 hex digits, once signed in */
@end

@implementation XivSignIn

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;

    /* the title, the gear at its right */
    UILabel* title = make_label(@"FINAL FANTASY XIV 1.23b", UIFontTextStyleTitle3, UIColor.labelColor);
    UIButtonConfiguration* gc = [UIButtonConfiguration plainButtonConfiguration];
    gc.image = [UIImage systemImageNamed:@"gearshape"];
    UIButton* gear = [UIButton buttonWithConfiguration:gc primaryAction:nil];
    gear.accessibilityLabel = @"Server";
    [gear addTarget:self action:@selector(showServer) forControlEvents:UIControlEventTouchUpInside];
    UIStackView* top = [[UIStackView alloc] initWithArrangedSubviews:@[ title, gear ]];
    top.alignment = UIStackViewAlignmentCenter;

    self.user = make_field(@"Username", NO, self);
    self.user.textContentType = UITextContentTypeUsername;
    self.user.text = [d stringForKey:@"xiv.user"];
    self.password = make_field(@"Password", YES, self);
    self.password.textContentType = UITextContentTypePassword;
    self.password.returnKeyType = UIReturnKeyGo;

    /* Remember password, then Sign In, on one row */
    self.remember = [UISwitch new];
    self.remember.on = [d objectForKey:@"xiv.remember"] ? [d boolForKey:@"xiv.remember"] : YES;
    if (self.remember.on && login_page(saved(@"xiv.server")) && self.user.text.length)
        self.password.text = key_load(login_page(saved(@"xiv.server")), self.user.text);
    UILabel* rl = make_label(@"Remember password", UIFontTextStyleSubheadline, UIColor.labelColor);
    self.spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    self.spinner.hidesWhenStopped = YES;
    UIButtonConfiguration* c = [UIButtonConfiguration filledButtonConfiguration];
    c.title = @"Sign In";
    self.go = [UIButton buttonWithConfiguration:c primaryAction:nil];
    [self.go addTarget:self action:@selector(signIn) forControlEvents:UIControlEventTouchUpInside];
    [self.go.widthAnchor constraintGreaterThanOrEqualToConstant:120].active = YES;
    UIView* gap = [UIView new];
    UIStackView* row = [[UIStackView alloc] initWithArrangedSubviews:@[ self.remember, rl, gap, self.spinner, self.go ]];
    row.alignment = UIStackViewAlignmentCenter;
    row.spacing = 8;

    self.status = make_label(nil, UIFontTextStyleFootnote, UIColor.systemRedColor);
    self.status.numberOfLines = 0;
    self.status.textAlignment = NSTextAlignmentCenter;

    UIStackView* form = [[UIStackView alloc] initWithArrangedSubviews:@[ top, self.user, self.password, row, self.status ]];
    form.axis = UILayoutConstraintAxisVertical;
    form.spacing = 8;
    [form setCustomSpacing:12 afterView:top];
    [form setCustomSpacing:12 afterView:self.password];
    place_form(self, form, 12);
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    if (!saved(@"xiv.server").length) /* the first run: the server first */
        [self showServer];
}

- (void)showServer
{
    XivServerSheet* sheet = [XivServerSheet new];
    __weak XivSignIn* me = self;
    sheet.done = ^{
        XivSignIn* s = me;
        if (s && !s.password.text.length && s.remember.on && login_page(saved(@"xiv.server")) && s.user.text.length)
            s.password.text = key_load(login_page(saved(@"xiv.server")), s.user.text);
    };
    UINavigationController* nav = [[UINavigationController alloc] initWithRootViewController:sheet];
    nav.modalPresentationStyle = UIModalPresentationFormSheet;
    [self presentViewController:nav animated:YES completion:nil];
}

- (BOOL)textFieldShouldReturn:(UITextField*)f
{
    if (f == self.user)
        [self.password becomeFirstResponder];
    else
        [self signIn];
    return NO;
}

- (void)busy:(BOOL)busy status:(NSString*)text
{
    self.go.enabled = !busy;
    busy ? [self.spinner startAnimating] : [self.spinner stopAnimating];
    self.status.text = text;
}

- (void)signIn
{
    NSString* login = login_page(saved(@"xiv.server"));
    NSString* user = [self.user.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSString* password = self.password.text ?: @"";
    if (!login)
    {
        [self showServer];
        return;
    }
    if (!user.length || !password.length)
    {
        [self busy:NO status:@"Enter your username and password."];
        return;
    }
    NSURL* url = [NSURL URLWithString:login];
    if (!url)
    {
        [self busy:NO status:@"The server address is not an address."];
        return;
    }
    [self.view endEditing:YES];
    [self busy:YES status:nil];
    self.status.textColor = UIColor.secondaryLabelColor;
    self.status.text = @"Signing in…";

    NSString* body = [NSString stringWithFormat:@"username=%@&password=%@", form_value(user), form_value(password)];
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = @"POST";
    req.timeoutInterval = 20;
    [req setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
    req.HTTPBody = [body dataUsingEncoding:NSUTF8StringEncoding];

    [[NSURLSession.sharedSession dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
        NSString* text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
        NSString *sid = nil, *why = nil;
        NSInteger code = [resp isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse*)resp).statusCode : 0;
        /* <x-sqexauth sid="<56 hex>" lang=... region=... utc=...>: signed in; no tag: refused */
        NSRegularExpression* tag = [NSRegularExpression regularExpressionWithPattern:@"<x-sqexauth\\b[^>]*\\bsid=\"([0-9A-Fa-f]{56})\""
                                                                             options:NSRegularExpressionCaseInsensitive error:nil];
        NSTextCheckingResult* m = text ? [tag firstMatchInString:text options:0 range:NSMakeRange(0, text.length)] : nil;
        if (m)
            sid = [text substringWithRange:[m rangeAtIndex:1]];
        else if (err)
            why = err.localizedDescription;
        else if (code == 200)
        {
            /* the form again, with the server's message in its error element when it has one */
            NSRegularExpression* e = [NSRegularExpression regularExpressionWithPattern:@"id=\"?err\"?[^>]*>\\s*([^<]*[^<\\s])"
                                                                               options:NSRegularExpressionCaseInsensitive error:nil];
            NSTextCheckingResult* em = text ? [e firstMatchInString:text options:0 range:NSMakeRange(0, text.length)] : nil;
            why = em ? [text substringWithRange:[em rangeAtIndex:1]] : @"The server did not sign you in. Check your username and password.";
        }
        else
            why = [NSString stringWithFormat:@"The login page answered something else (HTTP %ld).", (long)code];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!sid)
            {
                self.status.textColor = UIColor.systemRedColor;
                [self busy:NO status:why];
                return;
            }
            NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
            [d setObject:user forKey:@"xiv.user"];
            [d setBool:self.remember.on forKey:@"xiv.remember"];
            key_save(login, user, self.remember.on ? password : nil);
            self.status.textColor = UIColor.secondaryLabelColor;
            [self busy:YES status:@"Signed in. Starting the game…"];
            self.session = sid;
        });
    }] resume];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return UIInterfaceOrientationMaskLandscape;
}

@end

/* Shows the form until a sign-in succeeds; the lobby's host and the launch argument come back.
 * Returns 0 if the window could not be made. */
int ios_signin(char* server, size_t server_n, char* arg, size_t arg_n)
{
    @autoreleasepool
    {
        UIWindowScene* scene = nil;
        for (UIScene* s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:UIWindowScene.class])
                scene = (UIWindowScene*)s;
        UIWindow* window = scene ? [[UIWindow alloc] initWithWindowScene:scene]
                                 : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
        XivSignIn* form = [XivSignIn new];
        window.rootViewController = form;
        window.windowLevel = UIWindowLevelAlert;
        [window makeKeyAndVisible];
        while (!form.session)
            @autoreleasepool
            {
                [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            }
        snprintf(server, server_n, "%s", lobby_host().UTF8String);
        int ok = xiv_launch_arg(form.session.UTF8String, XIV_ARGS_TICK, arg, arg_n);
        window.hidden = YES;
        window.rootViewController = nil;
        return ok;
    }
}
