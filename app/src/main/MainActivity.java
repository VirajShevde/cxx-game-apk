// TODO: replace this with your app's actual Java package name (check
// AndroidManifest.xml / build.gradle applicationId). It must match the
// "com_yourcompany_tacticalshooter" segment of the JNI function name in
// game57.cxx exactly (dots become underscores there).
package com.example.cxxgame;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.widget.TextView;
import android.widget.ScrollView;
import android.graphics.Color;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;

// Subclasses SDL2's default activity so the game keeps all of SDLActivity's
// normal behavior; this only adds the photo-picker flow used by the profile
// portrait. Point AndroidManifest.xml's <activity> at this class instead of
// org.libsdl.app.SDLActivity for this to take effect.
public class MainActivity extends SDLActivity {

    private static final int PICK_IMAGE_REQUEST = 4201;

    // --- TEMPORARY CRASH CATCHER -----------------------------------------
    // Two layers so we catch the exception no matter which thread it comes
    // from: (1) a JVM-wide default handler for background/native-callback
    // threads (e.g. SDL's own game loop thread), and (2) a try/catch around
    // onCreate for synchronous crashes on the main thread before that
    // handler would normally even apply. Either way, the app shows the full
    // error as on-screen text instead of just closing - screenshot it.
    // Remove this whole block once the underlying crash is fixed.
    private static MainActivity currentInstance;

    static {
        final Thread.UncaughtExceptionHandler defaultHandler =
                Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(final Thread thread, final Throwable ex) {
                final MainActivity activity = currentInstance;
                if (activity != null) {
                    activity.runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            activity.showErrorScreen(ex);
                        }
                    });
                    // Give the UI thread a moment to actually render the
                    // error screen before anything else can tear the
                    // process down.
                    try { Thread.sleep(3000); } catch (InterruptedException ignored) {}
                } else if (defaultHandler != null) {
                    defaultHandler.uncaughtException(thread, ex);
                }
            }
        });
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        currentInstance = this;
        try {
            super.onCreate(savedInstanceState);
        } catch (Throwable t) {
            showErrorScreen(t);
        }
    }

    private void showErrorScreen(Throwable t) {
        StringWriter sw = new StringWriter();
        t.printStackTrace(new PrintWriter(sw));

        TextView tv = new TextView(this);
        tv.setText(sw.toString());
        tv.setTextColor(Color.WHITE);
        tv.setBackgroundColor(Color.BLACK);
        tv.setTextIsSelectable(true);
        tv.setPadding(24, 24, 24, 24);
        tv.setTextSize(12);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(tv);

        setContentView(scroll);
    }
    // -----------------------------------------------------------------------

    // Called from native code (game57.cxx: android_open_image_picker) when
    // the player taps the profile portrait. ACTION_OPEN_DOCUMENT is the
    // modern Storage Access Framework picker - it needs no storage
    // permission in the manifest, and lets the player browse the real
    // gallery/Files app/cloud providers, not just app-local files.
    public void openImagePicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/*");
        startActivityForResult(intent, PICK_IMAGE_REQUEST);
    }
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "cxxgame"
        };
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_IMAGE_REQUEST || resultCode != Activity.RESULT_OK || data == null) {
            return;
        }

        Uri uri = data.getData();
        if (uri == null) return;

        // The picked URI is a content:// reference (possibly from another
        // app/cloud provider), not a filesystem path - SDL_image on the
        // native side needs a real path, so copy the bytes into this app's
        // own storage first.
        String copiedPath = copyToAppStorage(uri);
        if (copiedPath != null) {
            nativeOnImagePicked(copiedPath);
        }
    }

    private String copyToAppStorage(Uri uri) {
        try {
            ContentResolver resolver = getContentResolver();
            InputStream in = resolver.openInputStream(uri);
            if (in == null) return null;

            File outFile = new File(getCacheDir(), "profile_portrait.jpg");
            OutputStream out = new FileOutputStream(outFile);
            byte[] buffer = new byte[8192];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
            out.flush();
            out.close();
            in.close();
            return outFile.getAbsolutePath();
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    // Implemented in game57.cxx (Java_..._MainActivity_nativeOnImagePicked)
    // - hands the copied file's real path to the native game so it can
    // load + center-crop it into the portrait texture.
    public native void nativeOnImagePicked(String path);
}
