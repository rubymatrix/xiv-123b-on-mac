/* The sign-in both screens share (ios_signin.m, mac_signin.m): the password in the keychain, the
 * post to the login page, the settings kept in the app's defaults. Foundation only. */
#pragma once
#import <Foundation/Foundation.h>

/* the app's defaults: xiv.server, xiv.login, xiv.user (strings), xiv.remember (bool, default yes) */
NSString* signin_saved(NSString* key);
BOOL signin_remember(void);

/* the password kept for a login page and username; nil when none */
NSString* signin_key_load(NSString* login, NSString* user);
/* kept (or, with nil, forgotten) */
void signin_key_save(NSString* login, NSString* user, NSString* password);

/* Posts the account to <login>/oauth/ffxiv/login/login.send (the retail route, as xiv_login.py
 * --kodama). done runs on the main queue with the 56-hex-digit session id, or nil and why not. */
void signin_post(NSString* login, NSString* user, NSString* password, void (^done)(NSString* session, NSString* why));

/* What a server says of itself in /.well-known/legacyxiv (the Umbra launcher's): its login page,
 * lobby and patch service. Looked for at what the player typed (a URL or host:port: only there), else
 * at the host's port 80, then https, then 17743. done runs on the main queue; nils when none answers. */
void signin_discover(NSString* server, void (^done)(NSString* login, NSString* lobby, NSString* patch));

/* The server's settings from its discovery: the login page kept unless the player typed their own
 * (xiv.login, and xiv.login_auto while it is the discovered one), and the lobby (xiv.lobby, cleared
 * when none is given). done runs on the main queue: whether the server answered. */
void signin_autofill(NSString* server, void (^done)(BOOL found));

/* the lobby to connect to: the discovered one, else the server's host */
NSString* signin_lobby(void);

/* the host a server address names: no scheme, no port, no path */
NSString* signin_host(NSString* server);

/* After a sign-in: the username and the switch kept, the password kept or forgotten */
void signin_keep(NSString* user, NSString* password, BOOL remember);
