import android.media.AudioManager;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.util.Log;
import java.lang.reflect.Method;
import java.io.FileInputStream;

public final class U6MediaPlayerProbe {
    public static void main(String[] args) throws Exception {
        Method prepare = MediaPlayer.class.getDeclaredMethod("_prepare");
        Method start = MediaPlayer.class.getDeclaredMethod("_start");
        prepare.setAccessible(true);
        start.setAccessible(true);
        MediaPlayer player = new MediaPlayer();
        try {
            Log.i("U6SinkProbe", "BEGIN " + args[1]);
            if (args.length > 2 && args[2].equals("music-attributes")) {
                player.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build());
            } else {
                player.setAudioStreamType(AudioManager.STREAM_MUSIC);
            }
            try (FileInputStream file = new FileInputStream(args[0])) {
                player.setDataSource(file.getFD());
            }
            // Exercise the native audio path without app_process subtitle/Activity plumbing.
            prepare.invoke(player);
            Log.i("U6SinkProbe", "START " + args[1]);
            start.invoke(player);
            Thread.sleep(3500);
            if (player.getCurrentPosition() <= 0) throw new IllegalStateException("no media progress");
            player.stop();
        } finally {
            player.release();
        }
        System.out.println("U6 native WAV probe OK");
    }
}
