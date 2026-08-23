# OP Embedded BLE Android Uploader

独立、无后端的 Android 图片上传器。应用内置静态 HTML 界面，原生 Java 层只负责：

- 按 OP Embedded Service UUID 扫描 BLE；
- 申请 Android 附近设备权限；
- 将网页生成的内容临时写入 App 缓存；
- 使用现有 offset + payload 分包协议上传到 ESP32。

支持多个屏幕 Profile 的 RGB565 单图与 PNG 序列。上传前必须在“屏幕方案”中选择与设备固件匹配的项；Profile 会同时决定逻辑分辨率、圆屏/矩形预览、RGB/BGR 顺序、字节序以及无线内容分区容量。当前包含：

| 屏幕方案 | 分辨率 | 颜色顺序 | 无线内容上限 |
| --- | ---: | --- | ---: |
| Waveshare 1.75C | 466 × 466 | RGB565 / little-endian | 28.94 MiB |
| M5Stack StopWatch | 466 × 466 | RGB565 / little-endian | 12.94 MiB |
| M5Stack CoreS3 | 320 × 240 | BGR565 / little-endian | 12.94 MiB |

视频在 Android 端仍限制为最多 `4 秒`、`64 帧`；FPS 选项保留 `8 / 12 / 20`，默认 `20 FPS`。容量不足时继续使用现有的“完整加速”或“裁切结尾”策略，不提供 Web 端生产用的放弃上传选项。

## 构建

在仓库根目录运行：

```powershell
bun run mobile:apk
```

首次构建会把 JDK 17、Android Command-Line Tools、Android 35 SDK 和 Gradle 下载到仓库根目录的 `.android-portable/`。不安装 Android Studio，也不写系统环境变量。

生成文件：

```text
dist/android/OP-Embedded-BLE-debug.apk
```

## 使用

1. 通过 USB 初始化支持 BLE 的 OP Embedded Studio 基础固件。
2. 确保电脑端已经断开 BLE；使用手机 BLE 传输时建议同时拔掉设备 USB 线，避免 USB 串口复位或占用设备链路。
3. 在 Android 手机安装 APK，并允许“附近设备”权限。
4. 选择图片、裁切后点击“上传到设备”；右上角可切换 USB / 蓝牙传输模式。

应用不访问网络，图片处理和传输均在手机本地完成。

## USB 固件烧录（测试功能）

应用现在也可以通过 Android USB OTG 烧录微雪 ESP32-S3-Touch-AMOLED-1.75C 的预编译固件：

1. 使用支持 USB Host/OTG 的 Android 手机连接开发板。
2. 打开底部“设置”，点击“固件烧录”，再选择“USB 固件”或“BLE 固件”。
3. 授予 USB 设备权限；如果设备没有自动进入下载模式，按住 `BOOT` 后重新插拔或点击复位。
4. 等待 USB、分区表和应用固件写入完成，设备会自动重启。

这里的“BLE 固件”仍然是通过 USB 烧录的设备固件，烧录完成后才可以使用本应用的 BLE 内容上传。当前只打包微雪 1.75C 的两套测试固件，其他屏幕方案暂不支持 Android 固件烧录。

视频或 PNG 序列超过设备内容分区时，手机端会按“容量不足时”设置自动适配：默认均匀抽帧并保持所选 FPS，使完整内容以更快速度播放；也可以选择保留开头连续帧并裁切结尾，维持原播放速度。

## 发布标签

Android 上传器使用独立版本线，当前版本为 `1.0.0`（`versionCode 15`）。发布时同时递增 `versionCode`、更新 `versionName`，并使用 `android-vX.Y.Z` Git 标签；GitHub Release 上传 `dist/android/OP-Embedded-BLE-debug.apk`。当前构建产物使用 debug 签名，适合测试和内部分发。历史 `v0.3.5` 标签保留不变。
