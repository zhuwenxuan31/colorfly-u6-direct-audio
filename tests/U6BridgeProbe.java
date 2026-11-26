import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import java.lang.reflect.Field;
import java.io.BufferedReader;
import java.io.FileReader;

public final class U6BridgeProbe {
    private static native int install(String clientPath);

    public static void main(String[] args) throws Exception {
        if ("startup".equals(args[0])) {
            boolean mapped = false;
            try (BufferedReader maps = new BufferedReader(new FileReader("/proc/self/maps"))) {
                for (String line; (line = maps.readLine()) != null;) {
                    if (line.endsWith(" " + args[1])) mapped = true;
                }
            }
            if (!mapped) throw new AssertionError("candidate runtime was not mapped");
        } else {
            System.load(args[0]);
            if (install(args[1]) != 0) throw new AssertionError("stock JNI bridge registration");
        }
        Field nativeTrack = AudioTrack.class.getDeclaredField("mNativeTrackInJavaObj");
        nativeTrack.setAccessible(true);
        boolean direct = "direct".equals(args[2]);
        int usage = args.length > 3 ? Integer.parseInt(args[3]) : AudioAttributes.USAGE_MEDIA;
        int content = args.length > 4 ? Integer.parseInt(args[4]) : AudioAttributes.CONTENT_TYPE_UNKNOWN;
        AudioAttributes attrs = new AudioAttributes.Builder().setUsage(usage).setContentType(content).build();
        for (int rate : new int[]{48000, 192000}) {
            AudioTrack track = new AudioTrack(
                attrs,
                new AudioFormat.Builder().setSampleRate(rate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT).build(),
                32768, AudioTrack.MODE_STREAM, 0);
            try {
                if (track.getState() != AudioTrack.STATE_INITIALIZED ||
                    track.getSampleRate() != rate || track.getAudioSessionId() <= 0)
                    throw new AssertionError("track parameters at " + rate);
                if ((nativeTrack.getLong(track) == 0) != direct)
                    throw new AssertionError("wrong output owner at " + rate);
                if (direct) {
                    if (track.setStereoVolume(0.3f, 0.7f) != AudioTrack.SUCCESS)
                        throw new AssertionError("track volume at " + rate);
                    try {
                        track.play();
                        throw new AssertionError("play opened without the direct service at " + rate);
                    } catch (IllegalStateException expected) {
                        if (!"U6 direct output is unavailable".equals(expected.getMessage()))
                            throw expected;
                    }
                }
                System.out.println("U6 probe " + args[2] + " " + rate + " OK");
            } finally {
                track.release();
            }
        }
    }
}
