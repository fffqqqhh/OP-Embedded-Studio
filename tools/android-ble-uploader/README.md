# OP Embedded BLE Android Uploader

独立、无后端的 Android 图片上传器。应用内置静态 HTML 界面，原生 Java 层只负责：

- 按 OP Embedded Service UUID 扫描 BLE；
- 申请 Android 附近设备权限；
- 将网页生成的内容临时写入 App 缓存；
- 使用现有 offset + payload 分包协议上传到 ESP32。

支持 Waveshare 1.75C 和 M5Stack StopWatch 的 `466 × 466` RGB565 单图与 20 FPS PNG 序列。Waveshare 的内容上限为约 28.94 MiB，StopWatch 的 16MB Flash 布局上限为约 12.94 MiB。两款设备使用相同的 OP Embedded BLE 内容协议；请先为对应开发板刷入匹配的 BLE 基础固件。

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
2. 确保电脑端已经断开 BLE。
3. 在 Android 手机安装 APK，并允许“附近设备”权限。
4. 选择图片、裁切并上传，App 会自动连接 OP Embedded BLE。

应用不访问网络，图片处理和传输均在手机本地完成。

视频或 PNG 序列超过设备内容分区时，手机端会按“容量不足时”设置自动适配：默认均匀抽帧并保持所选 FPS，使完整内容以更快速度播放；也可以选择保留开头连续帧并裁切结尾，维持原播放速度。

## 发布标签

Android 上传器使用独立版本线。发布时同时递增 `versionCode`、更新 `versionName`，并使用 `android-vX.Y.Z` Git 标签。历史 `v0.3.5` 标签保留不变。
