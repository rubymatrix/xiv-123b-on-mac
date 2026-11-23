package local.xiv123b.anywhere;

import android.content.Intent;

import org.libsdl.app.SDLActivity;

/* The game: SDL's activity, running libmain.so (tests/xivboot_android.c). The sign-in screen
 * (SignInActivity) starts it with the session, the server and the lobby, which become SDL_main's
 * arguments: --xiv-session <56 hex>, --xiv-server <host>, --xiv-lobby <host>. Started without them
 * (adb, a scripted run), run.args says what to do. */
public final class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected String[] getArguments() {
        Intent i = getIntent();
        String session = i != null ? i.getStringExtra("session") : null;
        if (session == null)
            return new String[0];
        String server = i.getStringExtra("server"), lobby = i.getStringExtra("lobby");
        return new String[] { "--xiv-session", session, "--xiv-server", server != null ? server : "",
                              "--xiv-lobby", lobby != null ? lobby : "" };
    }
}
