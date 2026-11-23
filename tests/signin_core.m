/* The sign-in both screens share. See signin_core.h. */
#import <Security/Security.h>
#import "signin_core.h"

static NSString* const kService = @"FINAL FANTASY XIV 1.23b sign-in";

/* --- the password in the keychain: one per login page and username --------------------------- */
static NSDictionary* key_query(NSString* login, NSString* user)
{
    return @{ (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
              (__bridge id)kSecAttrService : kService,
              (__bridge id)kSecAttrAccount : [NSString stringWithFormat:@"%@ %@", user, login] };
}

NSString* signin_key_load(NSString* login, NSString* user)
{
    NSMutableDictionary* q = [key_query(login, user) mutableCopy];
    q[(__bridge id)kSecReturnData] = @YES;
    CFTypeRef data = NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &data) != errSecSuccess || !data)
        return nil;
    return [[NSString alloc] initWithData:(__bridge_transfer NSData*)data encoding:NSUTF8StringEncoding];
}

void signin_key_save(NSString* login, NSString* user, NSString* password)
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

NSString* signin_host(NSString* server)
{
    NSString* s = [server stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSRange r = [s rangeOfString:@"://"];
    if (r.location != NSNotFound)
        s = [s substringFromIndex:NSMaxRange(r)];
    for (NSString* cut in @[ @"/", @":" ])
    {
        NSRange c = [s rangeOfString:cut];
        if (c.location != NSNotFound)
            s = [s substringToIndex:c.location];
    }
    return s;
}

NSString* signin_login_page(NSString* server)
{
    NSString* s = [server stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSRange r = [s rangeOfString:@"://"];
    NSString* scheme = r.location != NSNotFound ? [s substringToIndex:r.location] : @"http";
    if (r.location != NSNotFound)
        s = [s substringFromIndex:NSMaxRange(r)];
    NSRange slash = [s rangeOfString:@"/"];
    if (slash.location != NSNotFound)
        s = [s substringToIndex:slash.location];
    return s.length ? [NSString stringWithFormat:@"%@://%@/account/content/ffxivlogin", scheme, s] : nil;
}

NSString* signin_lobby(void)
{
    NSString* lobby = signin_saved(@"xiv.lobby");
    return lobby.length ? signin_host(lobby) : signin_host(signin_saved(@"xiv.server"));
}

NSString* signin_saved(NSString* key)
{
    return [[NSUserDefaults.standardUserDefaults stringForKey:key]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet] ?: @"";
}

BOOL signin_remember(void)
{
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
    return [d objectForKey:@"xiv.remember"] ? [d boolForKey:@"xiv.remember"] : YES;
}

void signin_keep(NSString* user, NSString* password, BOOL remember)
{
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
    [d setObject:user forKey:@"xiv.user"];
    [d setBool:remember forKey:@"xiv.remember"];
    NSString* login = signin_login_page(signin_saved(@"xiv.server"));
    if (login)
        signin_key_save(login, user, remember ? password : nil);
}

/* the first group of a pattern's first match in text, or nil */
static NSString* first_match(NSString* text, NSString* pattern)
{
    NSRegularExpression* re = [NSRegularExpression regularExpressionWithPattern:pattern options:NSRegularExpressionCaseInsensitive error:nil];
    NSTextCheckingResult* m = text ? [re firstMatchInString:text options:0 range:NSMakeRange(0, text.length)] : nil;
    return m ? [text substringWithRange:[m rangeAtIndex:1]] : nil;
}

NSString* signin_session_in(NSString* page)
{
    return first_match(page, @"<x-sqexauth\\b[^>]*\\bsid=\"([0-9A-Fa-f]{56})\"");
}

void signin_post(NSString* server, NSString* user, NSString* password, void (^done)(NSString* session, NSString* why))
{
    NSString* login = signin_login_page(server);
    NSURL* url = login ? [NSURL URLWithString:login] : nil;
    if (!url)
    {
        dispatch_async(dispatch_get_main_queue(), ^{ done(nil, @"The server address is not an address."); });
        return;
    }
    NSString* body = [NSString stringWithFormat:@"username=%@&password=%@", form_value(user), form_value(password)];
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = @"POST";
    req.timeoutInterval = 20;
    [req setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
    req.HTTPBody = [body dataUsingEncoding:NSUTF8StringEncoding];
    [[NSURLSession.sharedSession dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
        NSString* text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
        NSInteger code = [resp isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse*)resp).statusCode : 0;
        NSString *sid = signin_session_in(text), *why = nil;
        if (sid)
            ;
        else if (err)
            why = err.localizedDescription;
        else if (code == 200) /* the form again: the server's message in its error element, when it has one */
            why = first_match(text, @"id=\"?err\"?[^>]*>\\s*([^<]*[^<\\s])") ?: @"The server did not sign you in. Check your username and password.";
        else
            why = [NSString stringWithFormat:@"The login page answered something else (HTTP %ld).", (long)code];
        dispatch_async(dispatch_get_main_queue(), ^{ done(sid, why); });
    }] resume];
}
