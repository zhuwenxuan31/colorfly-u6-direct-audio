# 七彩虹 U6 原生音频直出

Experimental direct-to-DAC audio for the Colorfly U6 (firmware 2.0, Android 6.0.1).

这个项目尝试让七彩虹 U6 的原厂播放器和 Apple Music 把解码后的 PCM 送到内置 DAC，绕过 Android 的 AudioFlinger 和系统混音器，并保留原采样率。

> 我是个播放器爱好者，也是编程新手，不太懂程序。这套方案是我借助 AI 做的实机实验。欢迎懂 Android 音频、ALSA、tinyalsa 或全志平台的大神帮忙看代码、提 Issue 或发 PR，一起把它做得更稳定。

## 实机进度

| 播放场景 | 内置 DAC 输入 | 验证结果 |
| --- | --- | --- |
| Apple Music 下载版《青衣》 | 48 kHz / S32_LE，3,072 kbps | R23 断网实播 |
| Apple Music 下载版《New Kid In Town》 | 192 kHz / S32_LE，12,288 kbps | 早期工程版断网实播 |
| 原厂播放器本地 WAV | 44.1 kHz / S16_LE，1,411.2 kbps | R23 连续测得 25.22 秒，期间无缓冲欠载 |
| 原厂播放器 TF 卡 FLAC | 44.1 kHz / S16_LE，1,411.2 kbps | 早期工程版同一次开流推进约 35 秒 |

这些样本播放时，内置声卡由原生直出服务持有，AudioFlinger 无活跃音轨。表中的码率按双声道 PCM 容器计算。S32_LE 中的有效 24 位是否逐位保留，仍需数字样本验证。

源码对应 R23 实验阶段。实机使用 RAM 临时启动。循环暂停恢复、96 kHz 切换、第三方音乐 App 和系统提示音仍需完整验收。本仓库提供源码与测试，原厂固件、Apple Music APK 和设备镜像需要自行取得。

## 怎么看代码

| 目录 | 内容 |
| --- | --- |
| [src](src) | 共享 PCM 队列、格式转换、原厂 raw 输出、时钟与音量控制 |
| [android](android) | AudioTrack 和原生 MediaPlayer 接入、Binder 服务、实机核验脚本 |
| [deploy](deploy) | 从用户提供的原厂文件构建候选 boot 镜像的代码 |
| [tests](tests) | 主机测试和 Android 隔离探针 |

先运行 `make test` 检查 PCM 核心。交叉构建的依赖和命令见[构建说明](docs/build.md)。

## 欢迎一起完善

优先需要帮助的地方是 44.1/96 kHz 暂停和恢复的欠载、切歌后时钟稳定性、音量和静音时序，以及更多播放器的实机兼容。提交复现时，欢迎注明 U6 固件版本、App、采样率、操作步骤和去除个人信息后的 ALSA 或 AudioFlinger 日志。

我尤其希望懂底层音频的朋友帮忙确认，什么时候应该预填充缓冲、如何在不落回系统混音的情况下恢复，以及怎样在实机上验证有效位深。
