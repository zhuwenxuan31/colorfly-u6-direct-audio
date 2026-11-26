# 构建与验证

已验证设备为七彩虹 U6，系统版本 Android 6.0.1，构建号 `MOB30J.20191206`。原厂文件哈希不匹配时，构建脚本会退出。

## 主机测试

安装 Clang（含 AddressSanitizer、UndefinedBehaviorSanitizer）和 Make 后运行：

```sh
make test
```

该命令检查 PCM 格式转换、缓冲、会话、板级控制和占用逻辑，使用测试替身，无需连接 U6。

## Android 双架构构建

交叉构建脚本目前使用 NDK r27d 的 macOS 宿主工具路径，目标 Android API 23。还需要 Python 3.10 及以上、`pyelftools`、AOSP Android 6.0.1_r81 的头文件、与目标固件匹配的链接库。

| 环境变量 | 默认目录或文件 |
| --- | --- |
| `U6_NDK` | `vendor/ndk`，解包后的 NDK 根目录 |
| `U6_AOSP_HEADERS` | `vendor/aosp`，包含 `native/binder/Parcel.h`、`core/utils/RefBase.h`、`kernel/linux/binder.h` 的头文件目录 |
| `U6_STOCK_LINK_LIBS` | `vendor/stock-libs`，包含原厂 `lib/` 和 `lib64/` |
| `U6_BUILD_DIR` | `out/android`，构建结果 |
| `U6_ADB` | `adb`，设备检查和部署构建所用的 ADB |

将 [stock-libs.sha256](stock-libs.sha256) 复制为原厂链接库目录下的 `SHA256SUMS`。该清单同时列出所需文件名和准确版本。安装好依赖后执行：

```sh
python3 -m pip install pyelftools
sh android/build.sh
```

## 候选 boot 镜像

这一阶段还需要本机的原厂 `boot.fex`（`U6_STOCK_BOOT`，默认 `vendor/boot.fex`）、同版本原厂 SELinux 策略（`U6_STOCK_POLICY`，默认 `vendor/sepolicy`）、脚本校验的 ARM64 Magisk v27.0 `magiskpolicy`（`U6_POLICY_TOOL`，默认 `out/tools/magisk-v27/magiskpolicy`），以及已通过 ADB 连接的原厂 U6。构建会比对实机策略，在设备的临时目录生成中间文件。

```sh
U6_NATIVE_OUTPUT=1 sh deploy/build_candidate.sh
U6_NATIVE_OUTPUT=1 sh deploy/build_boot_overlay.sh out/deploy-candidate out/boot-overlay-candidate
```

以上命令生成候选镜像。候选镜像的实机验收使用可恢复的 RAM 临时启动流程，仓库当前没有提供一键安装包。现有设备核验脚本见 [android](../android) 和 [deploy](../deploy)。实机 Java 探针还依赖放在 `vendor/test-tools` 的 JDK 17.0.21+9、Android API 23 桩库 `android23.jar` 和 `r8-8.3.37.jar`。

## 实机直出判据

- 离线曲目在设备无默认网络时播放。
- ALSA 声卡处于 RUNNING，采样率与 PCM 容器符合预期。
- 内置声卡占用进程为直出服务，硬件帧与应用帧指针持续推进。
- 采样期间 AudioFlinger 没有活跃音轨。
- 循环暂停恢复需要检查整段日志，单次重新开流不足以证明连续播放没有欠载。
