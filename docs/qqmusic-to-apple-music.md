# 把 U6 原厂播放器的「QQ音乐」入口改成 Apple Music

在已验证的 U6 2.0 上，自带播放器的「QQ音乐」按钮会启动包名为 `com.tencent.qqmusic` 的应用。当时设备里的这个包是一个 12 KB 的本地启动桥 U6-entry-1.1。我们备份它，然后用相同包名安装自己签名的 U6-entry-1.3。自带播放器的按钮仍可能显示「QQ音乐」，按下后会打开设备已安装的 Apple Music（`com.apple.android.music`）。桥接包安装在数据分区，正常重启后保留，日常使用无需连接电脑。

桥接应用先用原厂 `dev_setting` 服务将 `setting.audio.player` 设为 0。已有 DIRECT 音轨时保留它。如果发现 Apple Music 正在走混音，入口尝试通过媒体会话停止并恢复播放，再核对新音轨。Apple Music 的原应用、账户和下载继续由 Apple Music 管理。

## 构建

准备自己备份的 U6-entry-1.1 启动桥 APK、JDK 17、Android API 23 的 `android.jar` 和 `r8-8.3.37.jar`。构建器会校验旧桥的 SHA-256，再用其入口清单和资源构建新桥。以下命令在仓库根目录执行。

```sh
python3 android/qqmusic-entry/build_entry.py \
  --template /path/to/U6-entry-1.1.apk \
  --java-home /path/to/jdk-17/Contents/Home \
  --android-jar /path/to/android23.jar \
  --r8-jar /path/to/r8-8.3.37.jar
```

成品位于 `out/qqmusic-entry/entry/U6-AppleMusic-Direct-Entry.apk`。保存自己生成的签名材料 `out/qqmusic-entry/signing/`，后续更新使用同一签名。验证记录所用的旧包 SHA-256 为 `7cfb4088d3e5b2f26fddd3af65123b5f9e6570e43d164dce6b69ea4c80c47157`。如果设备安装的是完整版 QQ 音乐或另一版本入口，先确认包来源和恢复方式。当前构建器只接受上述模板。

## 安装与回退

1. 用 `adb shell pm path com.tencent.qqmusic` 找到设备上旧桥的 APK 路径，再用 `adb pull` 备份。先确保手上的备份能用于回退。
2. 若新旧桥签名不同，Android 不接受覆盖安装。确认该包是已备份的旧启动桥后，才卸载 `com.tencent.qqmusic`。保留 `com.apple.android.music` 的安装和数据。
3. 在实测的 U6 固件上，通过 Android 安装会话 `pm install-create` → `pm install-write` → `pm install-commit` 安装 APK。下面的 `SESSION` 取自第一条命令返回的会话编号。

   ```sh
   apk=out/qqmusic-entry/entry/U6-AppleMusic-Direct-Entry.apk
   bytes=$(wc -c < "$apk" | tr -d '[:space:]')
   adb push "$apk" /data/local/tmp/u6-local-entry.apk
   adb shell pm install-create -r -S "$bytes"
   SESSION=123  # 用实际返回的会话编号替换 123
   adb shell pm install-write -S "$bytes" "$SESSION" base.apk /data/local/tmp/u6-local-entry.apk
   adb shell pm install-commit "$SESSION"
   adb shell rm /data/local/tmp/u6-local-entry.apk
   ```

4. 在实测固件上可一次性运行 `adb shell pm grant com.tencent.qqmusic android.permission.DUMP`，供桥接应用从 AudioFlinger 读取诊断信息。然后在原厂播放器内点「QQ音乐」，在 Apple Music 里开始播放。
5. 回退时卸载新桥，再用相同安装会话流程安装备份的旧桥。安装会话中断时可用 `adb shell pm install-abandon "$SESSION"` 放弃未提交的会话。

## 实测范围

正常重启后，从「QQ音乐」入口打开 Apple Music，曾实测到 **44.1 kHz／16 位 DIRECT**。AudioFlinger 的 DIRECT 线程与 ALSA 声卡占用一致，当时没有 Apple Music 的活跃 MIXER 音轨。测试时 Apple Music 已关闭交叉渐入渐出。其它采样率、浮点输出、通知栏和原 Apple Music 图标等其他入口仍需单独验证。

这份方法使用 U6 原厂 DIRECT 路由。仓库里的 R23 原生直出工程另有自己的 PCM 服务和实测记录。
