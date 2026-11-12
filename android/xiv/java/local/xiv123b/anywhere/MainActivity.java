package local.xiv123b.anywhere;

import org.libsdl.app.SDLActivity;

/* The app's activity: SDL's, running libmain.so (tests/xivboot_android.c). */
public final class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
