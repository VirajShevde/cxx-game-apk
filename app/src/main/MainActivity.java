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
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
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

    // --- TEMPORARY CRASH LOGGER ---------------------------------------
    // Catches whatever kills the app on startup and writes it to a plain
    // text file in the app's own external files dir, so it can be read
    // with any file manager app without needing adb/logcat/a PC.
    // Remove this block once the crash is fixed.
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        final Thread.UncaughtExceptionHandler defaultHandler =
                Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread thread, Throwable ex) {
                try {
                    File outDir = getExternalFilesDir(null);
                    if (outDir != null) {
                        File outFile = new File(outDir, "crash_log.txt");
                        StringWriter sw = new StringWriter();
                        ex.printStackTrace(new PrintWriter(sw));
                        FileWriter fw = new FileWriter(outFile, false);
                        fw.write(sw.toString());
                        fw.close();
                    }
                } catch (Exception loggingError) {
                    loggingError.printStackTrace();
                }
                if (defaultHandler != null) {
                    defaultHandler.uncaughtException(thread, ex);
                }
            }
        });

        try {
            super.onCreate(savedInstanceState);
        } catch (Throwable t) {
            // Catches crashes that happen synchronously during onCreate
            // (e.g. UnsatisfiedLinkError from loadLibrary) before the
            // uncaught handler above would otherwise fire.
            try {
                File outDir = getExternalFilesDir(null);
                if (outDir != null) {
                    File outFile = new File(outDir, "crash_log.txt");
                    StringWriter sw = new StringWriter();
                    t.printStackTrace(new PrintWriter(sw));
                    FileWriter fw = new FileWriter(outFile, false);
                    fw.write(sw.toString());
                    fw.close();
                }
            } catch (Exception loggingError) {
                loggingError.printStackTrace();
            }
            throw t;
        }
    }
    // --------------------------------------------------------------------

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
            "SDL2",
            "SDL2_image",
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
