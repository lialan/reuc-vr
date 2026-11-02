// Resident Evil: The Umbrella Chronicles VR for Meta Quest. All of the game runs natively (libmain.so, see game/android_main.cpp);
// this only checks that the player's game files are there and loads the libraries (HOTD4 VR's HouseOfTheDead4VR.java for this game).
//
// The game's files (the folder holding DATA/sys/main.dol) are looked for, as android_main.cpp does:
//   /sdcard/Android/data/org.uc.umbrellachronicles.vr/files/game   (the app's own folder)
//   /sdcard/UmbrellaChronicles/game                                 (where players copy it; needs all-files access, which the app asks
//                                                                    for when it finds no game)
// The APK holds the game's code translated from one exact main.dol (R.string.dol_sha256); any other version would not run, so it is
// checked here first. Without a usable game the app writes HOW-TO-ADD-THE-GAME.txt into its folder and closes.
package org.uc.umbrellachronicles;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.widget.Toast;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import org.libsdl.app.SDLActivity;

public class UmbrellaChroniclesVR extends SDLActivity {
    private static final String TAG = "UmbrellaChroniclesVR";
    private String problem;     /* null: the game is ready */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        problem = checkGame();
        // SDL3 (unlike HOTD4's SDL2) calls into libSDL3 when an activity is created again in the same process. After a refused start
        // (no game: loadLibraries threw) nothing was ever loaded, so a second start from the library crashed with UnsatisfiedLinkError
        // instead of checking the game again. Nothing native ran in this process: it is a first creation.
        if (mBrokenLibraries) { mActivityCreated = false; mSDLMainFinished = false; }
        super.onCreate(savedInstanceState);
        if (problem != null) {
            Toast.makeText(this, problem, Toast.LENGTH_LONG).show();
            finish();
        }
    }

    @Override
    public void loadLibraries() {
        if (problem != null) throw new RuntimeException(problem);   /* SDL then starts nothing */
        super.loadLibraries();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "openxr_loader", "main" };
    }

    @Override
    protected String[] getArguments() {
        String path = getIntent().getBooleanExtra("uc.require_private_game", false) ?
                getIntent().getStringExtra("uc.private_game") : null;
        return path == null ? super.getArguments() : new String[] { "--private-game", path };
    }

    private String checkGame() {
        File own = getExternalFilesDir(null);
        // Diagnostics must never silently run against the player's shared tree. Check in the real app process: shell-readable storage is
        // insufficient.
        if (getIntent().getBooleanExtra("uc.require_private_game", false)) {
            String path = getIntent().getStringExtra("uc.private_game");
            if (path == null || !path.matches(java.util.regex.Pattern.quote(getFilesDir().getAbsolutePath()) +
                    "/\\.codex-xr-[0-9a-f]{32}")) return "Invalid private diagnostic path";
            own = new File(path);
            File dol = new File(own, "game/DATA/sys/main.dol");
            File files = new File(own, "game/DATA/files");
            if (!dol.canRead() || !files.isDirectory() || files.list() == null ||
                    !getString(R.string.dol_sha256).trim().equalsIgnoreCase(sha256(dol))) {
                String error = "Private game diagnostic refused: app cannot read the staged game.";
                Log.e(TAG, error);
                return error;
            }
            Log.i(TAG, "private game verified: " + dol);
            return null;
        }

        File shared = new File(Environment.getExternalStorageDirectory(), "UmbrellaChronicles");
        File dol = null;
        for (File base : new File[] { own, shared }) {
            File f = base == null ? null : new File(base, "game/DATA/sys/main.dol");
            if (f != null && f.canRead()) { dol = f; break; }
        }
        String msg = null;
        if (dol == null) {
            if (!Environment.isExternalStorageManager()) {
                msg = "Allow Umbrella Chronicles VR to access all files, copy the game folder (game/DATA/...) to " + shared +
                      ", then start it again.";
                askForFileAccess();
            } else {
                msg = "Game files not found: copy the game folder (game/DATA/...) to " + shared;
            }
        } else {
            String want = getString(R.string.dol_sha256).trim();
            String have = sha256(dol);
            Log.i(TAG, "game: " + dol + ", main.dol " + have);
            if (!want.isEmpty() && !want.equalsIgnoreCase(have)) {
                msg = "This is a different version of the game (main.dol " + have.substring(0, 12) +
                      "), the app needs the NTSC-U release RBUE08 (main.dol " + want.substring(0, 12) + ").";
            }
        }
        writeHelp(own, msg);
        if (msg != null) Log.e(TAG, msg);
        return msg;
    }

    /* Called from the game: the settings panel's "Reset game". Restart starts it again once this process is gone. */
    public void restartGame() {
        Log.i(TAG, "reset: starting the game again");
        Intent again = new Intent(this, Restart.class);
        again.putExtra(Restart.PID, android.os.Process.myPid());
        again.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(again);
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private void askForFileAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                     Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            Log.w(TAG, "no all-files access screen: " + e);
        }
    }

    private static String sha256(File f) {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) md.update(buf, 0, n);
            StringBuilder s = new StringBuilder();
            for (byte b : md.digest()) s.append(String.format("%02x", b));
            return s.toString();
        } catch (Exception e) {
            return "unreadable";
        }
    }

    private void writeHelp(File own, String msg) {
        if (own == null) return;
        File help = new File(own, "HOW-TO-ADD-THE-GAME.txt");
        if (msg == null) { help.delete(); return; }
        String text =
            "Umbrella Chronicles VR needs your own copy of Resident Evil: The Umbrella Chronicles (Wii, NTSC-U, RBUE08).\n\n" +
            "Last start: " + msg + "\n\n" +
            "Unpack the disc (wit extract <image> game --pmode name) and copy the folder so that the game's main.dol ends up at\n\n" +
            "    /sdcard/UmbrellaChronicles/game/DATA/sys/main.dol\n\n" +
            "(Internal shared storage > UmbrellaChronicles over USB), and allow the app to access all files when it asks.\n\n" +
            "The app needs main.dol with SHA-256 " + getString(R.string.dol_sha256).trim() + "\n";
        try (FileOutputStream out = new FileOutputStream(help)) {
            out.write(text.getBytes("UTF-8"));
        } catch (Exception e) {
            Log.w(TAG, "cannot write " + help + ": " + e);
        }
    }
}
