package local.xiv123b.anywhere;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.text.InputType;
import android.util.Base64;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileReader;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.ArrayList;
import java.util.List;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/* The sign-in screen, before the game (MainActivity), as the iPhone's (tests/ios_signin.m with
 * tests/signin_core.m): the server's address, the account, and a switch to keep the password. The server
 * says where its login page and lobby are in /.well-known/legacyxiv; the account is posted to the login
 * page's retail route, which answers with a session id. The game gets the session, the server and the
 * lobby as its arguments (MainActivity.getArguments, then tests/xivboot_android.c).
 *
 * The password, when kept, is encrypted with a key in the Android keystore that never leaves it. A
 * run.args with XIV_ARGS (scripted runs, docs/android-port.md) skips this screen; a server.txt beside it
 * (the installer's configure) fills in the server the first time. */
public final class SignInActivity extends Activity {
    private static final String KEY_ALIAS = "xiv-signin";

    private SharedPreferences prefs;
    private EditText server, user, password, login;
    private CheckBox remember;
    private Button signIn;
    private TextView status;
    private volatile boolean busy;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        File files = getExternalFilesDir(null);
        if (files != null && runArgsHaveSession(new File(files, "run.args"))) {
            startGame(null, null, null);
            return;
        }
        prefs = getSharedPreferences("xiv", MODE_PRIVATE);
        buildScreen();
        String s = prefs.getString("server", "");
        if (s.isEmpty() && files != null)
            s = firstLine(new File(files, "server.txt"));
        server.setText(s);
        user.setText(prefs.getString("user", ""));
        remember.setChecked(prefs.getBoolean("remember", true));
        if (!prefs.getBoolean("login_auto", false))
            login.setText(prefs.getString("login", ""));
        String kept = loadPassword(prefs.getString("login", ""), user.getText().toString());
        if (kept != null)
            password.setText(kept);
        if (login.getText().length() > 0)
            login.setVisibility(View.VISIBLE); /* a login page the player typed: in sight */
    }

    // --- the screen -----------------------------------------------------------------------------

    /* For a phone in landscape, whose keyboard takes half the height: everything in the top rows, the
     * three fields side by side, so the one being typed in stays in sight with its neighbours. */
    private void buildScreen() {
        float dp = getResources().getDisplayMetrics().density;
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * dp);
        col.setPadding(pad, (int) (8 * dp), pad, pad);

        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = new TextView(this);
        title.setText("FINAL FANTASY XIV 1.23b");
        title.setTextSize(16);
        head.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        final TextView more = new TextView(this);
        more.setText("Server settings");
        more.setTextSize(13);
        more.setPadding(pad, (int) (6 * dp), 0, (int) (6 * dp));
        head.addView(more);
        col.addView(head, row(dp, 0));

        server = field("Server address", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        user = field("Username", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        password = field("Password", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        /* setSingleLine (in field) drops the password's dots: back on */
        password.setTransformationMethod(android.text.method.PasswordTransformationMethod.getInstance());
        password.setImeOptions(EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        password.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int action, android.view.KeyEvent ev) {
                signIn();
                return true;
            }
        });
        LinearLayout fields = new LinearLayout(this);
        fields.addView(server, weighted(dp, 1.2f, 0));
        fields.addView(user, weighted(dp, 1f, 12));
        fields.addView(password, weighted(dp, 1f, 12));
        col.addView(fields, row(dp, 4));

        remember = new CheckBox(this);
        remember.setText("Remember password");
        signIn = new Button(this);
        signIn.setText("Sign in");
        signIn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                signIn();
            }
        });
        status = new TextView(this);
        status.setGravity(Gravity.CENTER_VERTICAL);
        status.setPadding(pad, 0, pad, 0);
        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.CENTER_VERTICAL);
        actions.addView(remember);
        actions.addView(status, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        actions.addView(signIn);
        col.addView(actions, row(dp, 4));

        login = field("Login page (found from the server; type one to use your own)",
            InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        login.setTextSize(13);
        login.setVisibility(login.getText().length() > 0 ? View.VISIBLE : View.GONE);
        col.addView(login, row(dp, 4));
        more.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                login.setVisibility(login.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
            }
        });

        ScrollView scroll = new ScrollView(this);
        scroll.setFitsSystemWindows(true); /* below the status bar: Android 15 draws apps edge to edge */
        scroll.addView(col);
        setContentView(scroll);
    }

    private static LinearLayout.LayoutParams weighted(float dp, float weight, int left) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, weight);
        p.leftMargin = (int) (left * dp);
        return p;
    }

    private static LinearLayout.LayoutParams row(float dp, int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = (int) (top * dp);
        return p;
    }

    private EditText field(String hint, int type) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setInputType(type);
        e.setSingleLine(true);
        /* the form stays in sight in landscape, not the keyboard's full-screen text box */
        e.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        return e;
    }

    private void say(final String text, final boolean bad) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                status.setText(text);
                status.setTextColor(bad ? Color.rgb(220, 70, 60) : status.getHintTextColors().getDefaultColor());
            }
        });
    }

    // --- signing in -----------------------------------------------------------------------------

    private void signIn() {
        if (busy)
            return;
        final String s = server.getText().toString().trim();
        final String u = user.getText().toString().trim();
        final String pw = password.getText().toString();
        final String typedLogin = login.getText().toString().trim();
        final boolean keep = remember.isChecked();
        if (s.isEmpty() || u.isEmpty() || pw.isEmpty()) {
            say("Type the server's address, your username and your password.", true);
            return;
        }
        busy = true;
        signIn.setEnabled(false);
        say("Looking for the server...", false);
        new Thread(new Runnable() {
          @Override
          public void run() {
            try {
                String[] found = discover(s);
                String loginPage = !typedLogin.isEmpty() ? withScheme(typedLogin) : found[0];
                if (loginPage == null) {
                    say("The server did not say where its login page is. Check the address, or type the login "
                        + "page your server's people gave you.", true);
                    return;
                }
                final String lobby = found[1] != null ? found[1] : host(s);
                say("Signing in...", false);
                final String[] r = post(loginPage, u, pw);
                if (r[0] == null) {
                    say(r[1], true);
                    return;
                }
                SharedPreferences.Editor e = prefs.edit();
                e.putString("server", s).putString("user", u).putBoolean("remember", keep);
                e.putString("login", loginPage).putBoolean("login_auto", typedLogin.isEmpty());
                e.putString("lobby", lobby).apply();
                savePassword(loginPage, u, keep ? pw : null);
                final String session = r[0], lob = lobby;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        startGame(session, host(s), lob);
                    }
                });
            } catch (Exception ex) {
                say("Could not reach the server (" + ex.getClass().getSimpleName() + ").", true);
            } finally {
                busy = false;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        signIn.setEnabled(true);
                    }
                });
            }
          }
        }).start();
    }

    private void startGame(String session, String serverHost, String lobby) {
        Intent i = new Intent(this, MainActivity.class);
        if (session != null)
            i.putExtra("session", session).putExtra("server", serverHost).putExtra("lobby", lobby);
        startActivity(i);
        finish();
    }

    /* {login page, lobby host} from /.well-known/legacyxiv: at what was typed when it names a scheme
     * or port, else at the host's port 80, https, then 17743 (signin_core.m's signin_discover) */
    private static String[] discover(String typed) {
        List<String> bases = new ArrayList<>();
        String h = host(typed);
        if (typed.contains("://") || typed.contains(":")) {
            String b = withScheme(typed);
            int path = b.indexOf('/', b.indexOf("://") + 3);
            bases.add(path >= 0 ? b.substring(0, path) : b);
        } else if (!h.isEmpty()) {
            bases.add("http://" + h);
            bases.add("https://" + h);
            bases.add("http://" + h + ":17743");
        }
        for (String b : bases) {
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(b + "/.well-known/legacyxiv").openConnection();
                c.setConnectTimeout(4000);
                c.setReadTimeout(4000);
                if (c.getResponseCode() != 200)
                    continue;
                JSONObject j = new JSONObject(read(c.getInputStream()));
                String login = j.optString("login_server", "");
                if (login.isEmpty())
                    continue;
                String lobby = host(j.optString("lobby_server", ""));
                return new String[] { withScheme(login), lobby.isEmpty() ? null : lobby };
            } catch (Exception ignored) {
                // the next place
            }
        }
        return new String[] { null, null };
    }

    /* {session, null} or {null, why}: the account posted to <login>/oauth/ffxiv/login/login.send, the
     * retail route (signin_core.m's signin_post, tools/xiv_login.py --kodama) */
    private static String[] post(String login, String user, String password) throws Exception {
        while (login.endsWith("/"))
            login = login.substring(0, login.length() - 1);
        HttpURLConnection c = (HttpURLConnection) new URL(login + "/oauth/ffxiv/login/login.send").openConnection();
        c.setConnectTimeout(20000);
        c.setReadTimeout(20000);
        c.setRequestMethod("POST");
        c.setDoOutput(true);
        c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
        byte[] body = ("sqexid=" + URLEncoder.encode(user, "UTF-8") + "&password=" + URLEncoder.encode(password, "UTF-8"))
            .getBytes(StandardCharsets.UTF_8);
        try (OutputStream o = c.getOutputStream()) {
            o.write(body);
        }
        int code = c.getResponseCode();
        InputStream in = code < 400 ? c.getInputStream() : c.getErrorStream();
        String text = in != null ? read(in) : "";
        int ok = text.indexOf("login=auth,ok,sid,");
        if (ok >= 0 && text.length() >= ok + 18 + 56)
            return new String[] { text.substring(ok + 18, ok + 18 + 56), null };
        int ng = text.indexOf("login=auth,ng,err,");
        if (ng >= 0) {
            String rest = text.substring(ng + 18);
            int q = rest.indexOf('"');
            return new String[] { null, q >= 0 ? rest.substring(0, q) : rest };
        }
        return new String[] { null, "The login page did not answer as a FINAL FANTASY XIV login page (HTTP " + code + ")." };
    }

    // --- small things -----------------------------------------------------------------------------

    /* the host an address names: no scheme, port or path */
    static String host(String s) {
        s = s.trim();
        int i = s.indexOf("://");
        if (i >= 0)
            s = s.substring(i + 3);
        for (char cut : new char[] { '/', ':' }) {
            int c = s.indexOf(cut);
            if (c >= 0)
                s = s.substring(0, c);
        }
        return s;
    }

    private static String withScheme(String v) {
        if (v == null || v.isEmpty())
            return null;
        return v.startsWith("http://") || v.startsWith("https://") ? v : "http://" + v;
    }

    private static String read(InputStream in) throws Exception {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        for (int n; (n = in.read(buf)) > 0;)
            b.write(buf, 0, n);
        in.close();
        return b.toString("UTF-8");
    }

    private static String firstLine(File f) {
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String l = r.readLine();
            return l != null ? l.trim() : "";
        } catch (Exception e) {
            return "";
        }
    }

    private static boolean runArgsHaveSession(File f) {
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String l; (l = r.readLine()) != null;)
                if (l.startsWith("XIV_ARGS="))
                    return true;
        } catch (Exception ignored) {
            // no file
        }
        return false;
    }

    // --- the kept password: AES-GCM with a key that stays in the Android keystore -------------------

    private static SecretKey key() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (ks.containsAlias(KEY_ALIAS))
            return ((KeyStore.SecretKeyEntry) ks.getEntry(KEY_ALIAS, null)).getSecretKey();
        KeyGenerator g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        g.init(new KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .build());
        return g.generateKey();
    }

    private void savePassword(String loginPage, String u, String pw) {
        SharedPreferences.Editor e = prefs.edit().remove("pw").remove("pw_for");
        if (pw != null) {
            try {
                Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
                c.init(Cipher.ENCRYPT_MODE, key());
                byte[] iv = c.getIV(), ct = c.doFinal(pw.getBytes(StandardCharsets.UTF_8));
                e.putString("pw", Base64.encodeToString(iv, Base64.NO_WRAP) + ":" + Base64.encodeToString(ct, Base64.NO_WRAP));
                e.putString("pw_for", u + " " + loginPage);
            } catch (Exception ignored) {
                // not kept
            }
        }
        e.apply();
    }

    private String loadPassword(String loginPage, String u) {
        String v = prefs.getString("pw", null);
        if (v == null || !(u + " " + loginPage).equals(prefs.getString("pw_for", "")))
            return null;
        try {
            String[] parts = v.split(":");
            Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
            c.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(128, Base64.decode(parts[0], Base64.NO_WRAP)));
            return new String(c.doFinal(Base64.decode(parts[1], Base64.NO_WRAP)), StandardCharsets.UTF_8);
        } catch (Exception e) {
            return null;
        }
    }
}
