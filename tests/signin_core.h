/* The sign-in both screens share (ios_signin.m, mac_signin.m): the password in the keychain, the
 * post to the login page, the settings kept in the app's defaults. Foundation only. */
#pragma once
#import <Foundation/Foundation.h>

/* the app's defaults: xiv.server (host, or host:port), xiv.lobby (only when it is another host),
 * xiv.user (strings), xiv.remember (bool, default yes) */
NSString* signin_saved(NSString* key);
BOOL signin_remember(void);

/* the password kept for a login page and username; nil when none */
NSString* signin_key_load(NSString* login, NSString* user);
/* kept (or, with nil, forgotten) */
void signin_key_save(NSString* login, NSString* user, NSString* password);

/* The 1.23b login page on a server, as ffxivlogin.exe uses it and Meteor-PatchLogin-Server serves it:
 * http://<host[:port]>/account/content/ffxivlogin (https when the address says so). nil for none. */
NSString* signin_login_page(NSString* server);

/* Posts username and password to the server's login page. Its answer holds
 * <x-sqexauth sid="<56 hex>" lang=... region=... utc=...> when the account signed in; without the tag
 * it is a refusal, and the page's error message is the reason. done runs on the main queue with the
 * session id, or nil and why not. */
void signin_post(NSString* server, NSString* user, NSString* password, void (^done)(NSString* session, NSString* why));

/* the session id in a login page's answer (its <x-sqexauth> tag), or nil */
NSString* signin_session_in(NSString* page);

/* the lobby to connect to: xiv.lobby when set, else the server's host */
NSString* signin_lobby(void);

/* the host a server address names: no scheme, no port, no path */
NSString* signin_host(NSString* server);

/* After a sign-in: the username and the switch kept, the password kept or forgotten */
void signin_keep(NSString* user, NSString* password, BOOL remember);
