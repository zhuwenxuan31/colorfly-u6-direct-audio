package local.u6.appleentry;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.util.Log;
import android.widget.Toast;

/** Device-local entry; keeps the official Apple Music application unchanged. */
public final class EntryActivity extends Activity {
    private static final String TAG = "U6DirectEntry";
    private static final String TOKEN = "android.system.ISetting";

    private final android.os.Handler handler = new android.os.Handler();
    private android.media.browse.MediaBrowser browser;
    private android.media.session.MediaController controller;
    private boolean stopping;
    private boolean resumePlayback;
    private boolean resuming;
    private boolean restoredResumeIntent;
    private boolean done;
    private boolean connectionStarted;
    private boolean appleOpened;
    private final Runnable timeout = new Runnable() {
        @Override public void run() {
            fail(new IllegalStateException("Timed out waiting for Apple playback service"));
        }
    };
    private final android.media.session.MediaController.Callback callback =
            new android.media.session.MediaController.Callback() {
        @Override public void onPlaybackStateChanged(android.media.session.PlaybackState state) {
            if (state == null || done) return;
            if (resuming && state.getState() == 3) {
                Log.i(TAG, "Apple confirmed PLAYING after rebuild");
                verifyOutputThenOpen();
                return;
            }
            if (state.getState() == 7) {
                fail(new IllegalStateException("Apple playback reported an error"));
                return;
            }
            Log.i(TAG, "playback state=" + state.getState() + " stopping=" + stopping);
            if (stopping && (state.getState() == 1 || state.getState() == 0)) {
                if (state.getState() == 0) resumePlayback = false;
                resumeAndOpen();
            }
        }
        @Override public void onSessionDestroyed() {
            fail(new IllegalStateException("Apple playback session ended"));
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            Log.i(TAG, "start uid=" + android.os.Process.myUid()
                    + " package=" + getPackageName());
            if (state != null) {
                restoredResumeIntent = state.getBoolean("pendingResume", false);
            }
            setModeZero();
            if (getPackageName().equals("local.u6.directprobe")
                    && !getIntent().getBooleanExtra("u6.rebuildTest", false)) {
                Log.i(TAG, "probe complete; no Apple activity or audio command sent");
                done = true;
                finish();
                return;
            }
            if (!readAppleRoute().realTrack) {
                Log.i(TAG, "no existing Apple playback track; mode0 applied before future PLAY");
                launchApple();
                cleanup();
                finish();
                return;
            }
            android.widget.TextView text = new android.widget.TextView(this);
            text.setText("正在打开 Apple Music…");
            text.setGravity(android.view.Gravity.CENTER);
            setContentView(text);
            browser = new android.media.browse.MediaBrowser(this,
                    new ComponentName("com.apple.android.music",
                            "com.apple.android.music.player.MediaPlaybackService"),
                    new android.media.browse.MediaBrowser.ConnectionCallback() {
                @Override public void onConnected() {
                    if (done) return;
                    try {
                        controller = new android.media.session.MediaController(
                                EntryActivity.this, browser.getSessionToken());
                        controller.registerCallback(callback, handler);
                        android.media.session.PlaybackState current = controller.getPlaybackState();
                        int status = current == null ? 0 : current.getState();
                        Log.i(TAG, "connected playback state=" + status);
                        if (readAppleRoute().isDirect(status == 3)) {
                            Log.i(TAG, "existing Apple DIRECT track retained; no STOP needed");
                            openAppleAndFinish();
                            return;
                        }
                        resumePlayback = restoredResumeIntent || status == 3 || status == 6 || status == 8;
                        if (status == 0 || status == 1) {
                            resumeAndOpen();
                        } else {
                            stopping = true;
                            controller.getTransportControls().stop();
                            Log.i(TAG, "targeted STOP requested; waiting for stopped state");
                        }
                    } catch (Exception error) { fail(error); }
                }
                @Override public void onConnectionFailed() {
                    if (done) return;
                    Log.i(TAG, "playback service not ready; retrying within startup deadline");
                    browser.disconnect();
                    handler.postDelayed(new Runnable() {
                        @Override public void run() { if (!done) browser.connect(); }
                    }, 300);
                }
                @Override public void onConnectionSuspended() {
                    fail(new IllegalStateException("Apple MediaBrowser connection suspended"));
                }
            }, null);
            handler.postDelayed(timeout, 12000);
            launchApple();
        } catch (Exception error) { fail(error); }
    }

    @Override protected void onStop() {
        super.onStop();
        if (browser != null && !done && !connectionStarted) {
            connectionStarted = true;
            browser.connect();
        }
    }

    private void resumeAndOpen() {
        if (done) return;
        try {
            setModeZero();
            if (resumePlayback) {
                stopping = false;
                resuming = true;
                controller.getTransportControls().play();
                Log.i(TAG, "targeted PLAY requested; waiting for PLAYING state");
                return;
            } else {
                Log.i(TAG, "preserving non-playing state; next user PLAY creates new track");
            }
            openAppleAndFinish();
        } catch (Exception error) { fail(error); }
    }

    private void verifyOutputThenOpen() {
        if (done) return;
        try {
            if (readAppleRoute().isDirect(true)) {
                Log.i(TAG, "verified Apple DIRECT track after rebuild");
                openAppleAndFinish();
            } else {
                handler.postDelayed(new Runnable() {
                    @Override public void run() { verifyOutputThenOpen(); }
                }, 500);
            }
        } catch (Exception error) { fail(error); }
    }

    private String dumpService(String name, String[] args) throws Exception {
        IBinder service = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("getService", String.class).invoke(null, name);
        if (service == null) throw new IllegalStateException("Diagnostic service unavailable");
        java.io.File file = new java.io.File(getCacheDir(), "audio-route.tmp");
        try {
            java.io.FileOutputStream out = new java.io.FileOutputStream(file);
            try { service.dump(out.getFD(), args); } finally { out.close(); }
            if (file.length() > 1048576) throw new IllegalStateException("Unexpected diagnostic size");
            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
            java.io.FileInputStream in = new java.io.FileInputStream(file);
            try {
                byte[] buffer = new byte[4096];
                int size;
                while ((size = in.read(buffer)) != -1) bytes.write(buffer, 0, size);
            } finally { in.close(); }
            String result = bytes.toString("UTF-8");
            if (result.contains("Permission Denial")) {
                throw new IllegalStateException("Audio diagnostic permission unavailable");
            }
            return result;
        } finally { file.delete(); }
    }

    private static final class AudioRoute {
        boolean realTrack;
        boolean directTrack;
        boolean activeDirect;
        boolean activeMixed;
        boolean isDirect(boolean requireActive) {
            return directTrack && !activeMixed && (!requireActive || activeDirect);
        }
    }

    private AudioRoute readAppleRoute() throws Exception {
        AudioRoute route = new AudioRoute();
        String processes = dumpService("activity", new String[]{"processes"});
        java.util.regex.Matcher pid = java.util.regex.Pattern.compile(
                "ProcessRecord\\{[^}]*? (\\d+):com\\.apple\\.android\\.music/")
                .matcher(processes);
        if (!pid.find()) return route;
        String target = pid.group(1);
        String audio = dumpService("media.audio_flinger", new String[0]);
        boolean direct = false;
        boolean output = false;

        for (String line : audio.split("\\r?\\n")) {
            if (line.startsWith("Output thread ")) {
                output = true;
                direct = line.contains("(DIRECT)");
            } else if (line.startsWith("Input thread ") || line.startsWith("Record thread ")
                    || line.startsWith("Reroute ")) {
                output = false;
            }
            if (!output) continue;
            String[] fields = line.trim().split("\\s+");
            if (fields.length >= 11 && target.equals(fields[2]) && "3".equals(fields[3])
                    && ("yes".equals(fields[1]) || "no".equals(fields[1]))) {
                // Exo keeps one-frame/4000Hz dummy tracks solely for session IDs.
                if ("1".equals(fields[7]) && "4000".equals(fields[10])) continue;
                route.realTrack = true;
                boolean active = "yes".equals(fields[1]);
                if (direct) route.directTrack = true;
                if (direct && active) route.activeDirect = true;
                if (!direct && active) route.activeMixed = true;
            }
        }
        Log.i(TAG, "route pid=" + target + " real=" + route.realTrack
                + " direct=" + route.directTrack + " activeDirect=" + route.activeDirect
                + " activeMixed=" + route.activeMixed);
        return route;
    }

    private void openAppleAndFinish() {
        if (done) return;
        try {
            if (!appleOpened) launchApple();
            Log.i(TAG, "entry finished; original Apple activity is open");
            cleanup();
            finish();
        } catch (Exception error) { fail(error); }
    }

    private void launchApple() {
        Intent launch = new Intent();
        launch.setComponent(new ComponentName("com.apple.android.music",
                "com.apple.android.music.onboarding.activities.SplashActivity"));
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(launch);
        appleOpened = true;
        Log.i(TAG, "Apple activity opened before connecting playback service");
    }

    private void fail(Exception error) {
        if (done) return;
        Log.e(TAG, "entry failed", error);
        Toast.makeText(this, "未确认直出，请完全退出 Apple Music 后重试。", Toast.LENGTH_LONG).show();
        cleanup();
        finish();
    }

    private void cleanup() {
        done = true;
        handler.removeCallbacksAndMessages(null);
        if (controller != null) controller.unregisterCallback(callback);
        if (browser != null) browser.disconnect();
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        state.putBoolean("pendingResume", resumePlayback && !done);
        super.onSaveInstanceState(state);
    }

    @Override protected void onDestroy() {
        cleanup();
        super.onDestroy();
    }

    private void setModeZero() throws Exception {
        Class<?> manager = Class.forName("android.os.ServiceManager");
        IBinder service = (IBinder) manager.getMethod("getService", String.class)
                .invoke(null, "dev_setting");
        if (service == null || !TOKEN.equals(service.getInterfaceDescriptor())) {
            throw new IllegalStateException("Expected U6 setting service unavailable");
        }
        Parcel request = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            request.writeInterfaceToken(TOKEN);
            request.writeInt(0);
            if (!service.transact(4, request, reply, 0) || reply.dataAvail() < 4) {
                throw new IllegalStateException("Mode transaction not accepted");
            }
            // This OEM native service returns a raw status, no Java exception header.
            int status = reply.readInt();
            Class<?> props = Class.forName("android.os.SystemProperties");
            String mode = (String) props.getMethod("get", String.class, String.class)
                    .invoke(null, "setting.audio.player", "unknown");
            Log.i(TAG, "mode status=" + status + " readback=" + mode);
            if (status != 0 || !"0".equals(mode)) {
                throw new IllegalStateException("Mode0 readback failed");
            }
        } finally {
            reply.recycle();
            request.recycle();
        }
    }
}
