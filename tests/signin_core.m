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

static NSString* with_scheme(NSString* v)
{
    if (![v isKindOfClass:NSString.class] || !v.length)
        return nil;
    return [v hasPrefix:@"http://"] || [v hasPrefix:@"https://"] ? v : [@"http://" stringByAppendingString:v];
}

/* the next base's /.well-known/legacyxiv, until one answers with a login server */
static void discover_at(NSURLSession* session, NSArray<NSString*>* bases, NSUInteger i,
                        void (^done)(NSString* login, NSString* lobby, NSString* patch))
{
    if (i >= bases.count)
    {
        dispatch_async(dispatch_get_main_queue(), ^{ done(nil, nil, nil); });
        return;
    }
    NSURL* url = [NSURL URLWithString:[bases[i] stringByAppendingString:@"/.well-known/legacyxiv"]];
    [[session dataTaskWithURL:url completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
        NSDictionary* j = nil;
        if (data && !err && [resp isKindOfClass:NSHTTPURLResponse.class] && ((NSHTTPURLResponse*)resp).statusCode == 200)
            j = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (![j isKindOfClass:NSDictionary.class] || !j[@"login_server"])
        {
            discover_at(session, bases, i + 1, done);
            return;
        }
        NSString* login = with_scheme(j[@"login_server"]);
        NSString* lobby = [j[@"lobby_server"] isKindOfClass:NSString.class] ? signin_host(j[@"lobby_server"]) : nil;
        NSString* patch = with_scheme(j[@"game_patch_server"]);
        dispatch_async(dispatch_get_main_queue(), ^{ done(login, lobby.length ? lobby : nil, patch); });
    }] resume];
}

void signin_discover(NSString* server, void (^done)(NSString* login, NSString* lobby, NSString* patch))
{
    NSString* s = [server stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSString* host = signin_host(s);
    NSMutableArray<NSString*>* bases = [NSMutableArray array];
    BOOL exact = [s rangeOfString:@"://"].location != NSNotFound || [s rangeOfString:@":"].location != NSNotFound;
    if (exact)
    {
        NSString* b = with_scheme(s);
        NSRange path = [b rangeOfString:@"/" options:0 range:NSMakeRange([b rangeOfString:@"://"].location + 3, b.length - [b rangeOfString:@"://"].location - 3)];
        [bases addObject:path.location != NSNotFound ? [b substringToIndex:path.location] : b];
    }
    else if (host.length)
        [bases addObjectsFromArray:@[ [@"http://" stringByAppendingString:host], [@"https://" stringByAppendingString:host],
                                      [NSString stringWithFormat:@"http://%@:17743", host] ]];
    NSURLSessionConfiguration* cfg = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    cfg.timeoutIntervalForRequest = 4;
    discover_at([NSURLSession sessionWithConfiguration:cfg], bases, 0, done);
}

void signin_autofill(NSString* server, void (^done)(BOOL found))
{
    signin_discover(server, ^(NSString* login, NSString* lobby, NSString* patch) {
        (void)patch;
        NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
        if (login && (!signin_saved(@"xiv.login").length || [d boolForKey:@"xiv.login_auto"]))
        {
            [d setObject:login forKey:@"xiv.login"];
            [d setBool:YES forKey:@"xiv.login_auto"];
        }
        if (lobby)
            [d setObject:lobby forKey:@"xiv.lobby"];
        else
            [d removeObjectForKey:@"xiv.lobby"];
        done(login != nil);
    });
}

NSString* signin_lobby(void)
{
    NSString* lobby = signin_saved(@"xiv.lobby");
    return lobby.length ? lobby : signin_host(signin_saved(@"xiv.server"));
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
    signin_key_save(signin_saved(@"xiv.login"), user, remember ? password : nil);
}

void signin_post(NSString* login, NSString* user, NSString* password, void (^done)(NSString* session, NSString* why))
{
    if (![login hasPrefix:@"http://"] && ![login hasPrefix:@"https://"])
        login = [@"http://" stringByAppendingString:login];
    while ([login hasSuffix:@"/"])
        login = [login substringToIndex:login.length - 1];
    NSURL* url = [NSURL URLWithString:[login stringByAppendingString:@"/oauth/ffxiv/login/login.send"]];
    if (!url)
    {
        dispatch_async(dispatch_get_main_queue(), ^{ done(nil, @"The login page is not an address."); });
        return;
    }
    NSString* body = [NSString stringWithFormat:@"sqexid=%@&password=%@", form_value(user), form_value(password)];
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = @"POST";
    req.timeoutInterval = 20;
    [req setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
    req.HTTPBody = [body dataUsingEncoding:NSUTF8StringEncoding];
    [[NSURLSession.sharedSession dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
        NSString* text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
        NSString *sid = nil, *why = nil;
        NSRange ok = text ? [text rangeOfString:@"login=auth,ok,sid,"] : NSMakeRange(NSNotFound, 0);
        if (ok.location != NSNotFound && text.length >= NSMaxRange(ok) + 56)
            sid = [text substringWithRange:NSMakeRange(NSMaxRange(ok), 56)];
        else if (text && [text rangeOfString:@"login=auth,ng,err,"].location != NSNotFound)
        {
            NSString* rest = [text substringFromIndex:NSMaxRange([text rangeOfString:@"login=auth,ng,err,"])];
            NSRange q = [rest rangeOfString:@"\""];
            why = q.location != NSNotFound ? [rest substringToIndex:q.location] : rest;
        }
        else if (err)
            why = err.localizedDescription;
        else
            why = [NSString stringWithFormat:@"The login page answered something else (HTTP %ld).",
                            (long)((NSHTTPURLResponse*)resp).statusCode];
        dispatch_async(dispatch_get_main_queue(), ^{ done(sid, why); });
    }] resume];
}
