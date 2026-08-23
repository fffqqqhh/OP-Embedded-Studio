package app.openpencil.bleuploader;

import android.net.Uri;
import android.content.Intent;
import android.content.ClipData;
import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.BroadcastReceiver;
import android.content.pm.PackageManager;
import android.content.IntentFilter;
import android.app.PendingIntent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.provider.Settings;
import android.provider.MediaStore;
import android.util.Base64;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebResourceResponse;
import android.webkit.MimeTypeMap;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Collections;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

public final class MainActivity extends Activity {
    private static final int BLE_PERMISSION_REQUEST = 101;
    private static final UUID SERVICE_UUID = UUID.fromString("a110207d-8f4d-559b-8e4a-4791892b127d");
    private static final UUID TRANSFER_UUID = UUID.fromString("a210207d-8f4d-559b-8e4a-4791892b127d");
    private static final UUID STATUS_UUID = UUID.fromString("a310207d-8f4d-559b-8e4a-4791892b127d");
    private static final UUID CLIENT_CONFIG_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final int DEFAULT_PAYLOAD_CHUNK_BYTES = 16;
    private static final int MAX_PAYLOAD_CHUNK_BYTES = 505;
    private static final int FALLBACK_PAYLOAD_CHUNK_BYTES = 240;
    // Checkpoints confirm the stream and recover offsets after a failure. They
    // do not need to run for every small batch of BLE packets.
    private static final int STATUS_CHECKPOINT_BYTES = 64 * 1024;
    private static final long STATUS_READ_SETTLE_MS = 80L;
    private static final long WRITE_CALLBACK_TIMEOUT_MS = 1200L;
    private static final String CONTENT_COMPLETE_MESSAGE = "内容已接收，正在切换画面";
    private static final int PRIMARY_MTU_REQUEST = 517;
    private static final int FALLBACK_MTU_REQUEST = 247;
    private static final String DEFAULT_PROFILE_ID = "co5300_waveshare_amoled_1_75c";
    private static final int LARGE_FLASH_CONTENT_BYTES = 0xcf0000;
    private static final int WAVESHARE_CONTENT_BYTES = 0x1cf0000;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private WebView webView;
    private BluetoothAdapter bluetoothAdapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic transferCharacteristic;
    private BluetoothGattCharacteristic statusCharacteristic;
    private UsbManager usbManager;
    private boolean firmwareFlashing;
    private boolean usbContentUploading;
    private String pendingFirmwareMode;
    private boolean pendingConnect;
    private boolean scanning;
    private boolean connected;
    private boolean linkReady;
    private boolean statusSubscriptionPending;
    private boolean initialStatusReadPending;
    private int negotiatedMtu = 23;
    private int mtuRequestIndex;
    private Uri cameraCaptureUri;
    private File payloadFile;
    private FileOutputStream payloadOutput;
    private int payloadExpectedBytes;
    private int payloadWrittenBytes;
    private String selectedProfileId = DEFAULT_PROFILE_ID;
    private RandomAccessFile uploadInput;
    private int uploadTotal;
    private int lastProgressBytes;
    private int payloadChunkBytes = DEFAULT_PAYLOAD_CHUNK_BYTES;
    private boolean uploading;
    private int queuedOffset;
    private int confirmedOffset;
    private int checkpointReadAttempts;
    private int completionReadAttempts;
    private boolean checkpointPending;
    private boolean statusReadPending;
    private boolean awaitingCompletion;
    private boolean writePending;
    private int pendingWriteOffset;
    private int pendingWriteLength;
    private long statusReadNotBefore;
    private long statusReadDeadline;
    private long uploadStartedAt;

    private static final String USB_PERMISSION_ACTION = "app.openpencil.bleuploader.USB_PERMISSION";

    private final BroadcastReceiver usbPermissionReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (!USB_PERMISSION_ACTION.equals(intent.getAction())) return;
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
            String mode = pendingFirmwareMode;
            pendingFirmwareMode = null;
            if (!granted || device == null || mode == null) {
                if ("content".equals(mode)) emitError("没有获得 USB 设备权限");
                else emitFirmwareEvent("firmware-error", "没有获得 USB 设备权限");
                return;
            }
            if ("content".equals(mode)) startUsbContentUpload(device);
            else startFirmwareFlash(device, mode);
        }
    };

    @SuppressLint({"SetJavaScriptEnabled", "JavascriptInterface"})
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        BluetoothManager manager = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        bluetoothAdapter = manager == null ? null : manager.getAdapter();
        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        IntentFilter usbFilter = new IntentFilter(USB_PERMISSION_ACTION);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(usbPermissionReceiver, usbFilter, RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(usbPermissionReceiver, usbFilter);
        }

        webView = new WebView(this);
        WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setMediaPlaybackRequiresUserGesture(false);
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, String url) {
                return interceptAppAsset(Uri.parse(url));
            }

            @Override
            public WebResourceResponse shouldInterceptRequest(
                    WebView view, android.webkit.WebResourceRequest request) {
                return interceptAppAsset(request.getUrl());
            }
        });
        webView.addJavascriptInterface(new NativeBridge(), "OpenPencilNative");
        setContentView(webView);
        webView.loadUrl("https://appassets.androidplatform.net/assets/index.html");
    }

    private WebResourceResponse interceptAppAsset(Uri uri) {
        if (uri == null || !"appassets.androidplatform.net".equals(uri.getHost())) return null;
        String path = uri.getPath();
        if (path == null) return missingResource();
        try {
            if (path.startsWith("/assets/")) {
                String assetPath = path.substring("/assets/".length());
                if (assetPath.isEmpty() || assetPath.contains("..")) return missingResource();
                return resourceResponse(assetPath, getAssets().open(assetPath));
            }
            if (path.startsWith("/media/")) {
                String name = path.substring("/media/".length());
                if (name.isEmpty() || name.contains("/") || name.contains("\\") || name.contains("..")) {
                    return missingResource();
                }
                File file = new File(getCacheDir(), name);
                if (!file.isFile() || file.length() == 0) return missingResource();
                return resourceResponse(name, new java.io.FileInputStream(file));
            }
        } catch (IOException ignored) {
        }
        return missingResource();
    }

    private WebResourceResponse resourceResponse(String name, InputStream stream) {
        String extension = MimeTypeMap.getFileExtensionFromUrl(name);
        String mimeType = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension);
        if (mimeType == null) mimeType = "application/octet-stream";
        String encoding = mimeType.startsWith("text/") || "application/javascript".equals(mimeType)
                ? "UTF-8" : null;
        return new WebResourceResponse(mimeType, encoding, stream);
    }

    private WebResourceResponse missingResource() {
        return new WebResourceResponse("text/plain", "UTF-8",
                new ByteArrayInputStream(new byte[0]));
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_CAMERA_CAPTURE) {
            File captureFile = CameraFileProvider.captureFile(this);
            if (resultCode == RESULT_OK && captureFile.isFile() && captureFile.length() > 0) {
                deliverManagedMedia(captureFile.getName(), "image/jpeg");
            } else if (resultCode != RESULT_CANCELED) {
                emitError("拍照失败，没有收到图片");
            }
            cameraCaptureUri = null;
            return;
        }
        if (requestCode == REQUEST_PICK_MEDIA && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) importMedia(uri, getContentResolver().getType(uri));
            return;
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != BLE_PERMISSION_REQUEST) return;
        boolean granted = true;
        for (int result : results) granted &= result == PackageManager.PERMISSION_GRANTED;
        if (granted && pendingConnect) startBleScan();
        else emitError("需要附近设备权限才能扫描 OP Embedded BLE");
        pendingConnect = false;
    }

    private boolean hasBlePermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
                    && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
        }
        return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestBlePermissions() {
        pendingConnect = true;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            requestPermissions(
                    new String[]{Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT},
                    BLE_PERMISSION_REQUEST);
        } else {
            requestPermissions(new String[]{Manifest.permission.ACCESS_FINE_LOCATION}, BLE_PERMISSION_REQUEST);
        }
    }

    @SuppressLint("MissingPermission")
    private void startBleScan() {
        if (bluetoothAdapter == null) {
            emitError("这台手机不支持蓝牙");
            return;
        }
        if (!bluetoothAdapter.isEnabled()) {
            emitError("请先打开手机蓝牙");
            return;
        }
        if (connected) {
            emitEvent("connected", "已连接 OP Embedded BLE", -1, -1);
            return;
        }
        scanner = bluetoothAdapter.getBluetoothLeScanner();
        if (scanner == null) {
            emitError("无法启动 BLE 扫描，请重新打开蓝牙");
            return;
        }
        stopScan();
        scanning = true;
        emitEvent("status", "正在按 Service UUID 扫描设备…", -1, -1);
        ScanFilter filter = new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SERVICE_UUID)).build();
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        scanner.startScan(Collections.singletonList(filter), settings, scanCallback);
        mainHandler.postDelayed(() -> {
            if (!connected && scanning) {
                stopScan();
                emitError("没有发现 OP Embedded BLE，请确认设备未连接电脑");
            }
        }, 12000);
    }

    @SuppressLint("MissingPermission")
    private void stopScan() {
        if (scanner != null && scanning) scanner.stopScan(scanCallback);
        scanning = false;
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @SuppressLint("MissingPermission")
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            stopScan();
            emitEvent("status", "发现设备，正在连接…", -1, -1);
            gatt = result.getDevice().connectGatt(
                    MainActivity.this,
                    false,
                    gattCallback,
                    BluetoothDevice.TRANSPORT_LE);
        }

        @Override
        public void onScanFailed(int errorCode) {
            scanning = false;
            emitError("BLE 扫描失败：" + errorCode);
        }
    };

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        @Override
        public void onConnectionStateChange(BluetoothGatt nextGatt, int status, int newState) {
            if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) {
                gatt = nextGatt;
                connected = true;
                linkReady = false;
                statusSubscriptionPending = false;
                initialStatusReadPending = false;
                negotiatedMtu = 23;
                payloadChunkBytes = DEFAULT_PAYLOAD_CHUNK_BYTES;
                mtuRequestIndex = 0;
                emitDiagnostic("LINK: connected, discovering services");
                nextGatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    nextGatt.setPreferredPhy(
                            BluetoothDevice.PHY_LE_2M_MASK,
                            BluetoothDevice.PHY_LE_2M_MASK,
                            BluetoothDevice.PHY_OPTION_NO_PREFERRED);
                }
                emitEvent("status", "已连接，正在发现传输服务…", -1, -1);
                nextGatt.discoverServices();
                return;
            }
            connected = false;
            linkReady = false;
            statusSubscriptionPending = false;
            initialStatusReadPending = false;
            payloadChunkBytes = DEFAULT_PAYLOAD_CHUNK_BYTES;
            mtuRequestIndex = 0;
            transferCharacteristic = null;
            statusCharacteristic = null;
            stopUpload("BLE 已断开");
            emitDiagnostic("LINK: disconnected status=" + status + " state=" + newState);
            emitEvent("disconnected", "BLE 已断开", -1, -1);
            nextGatt.close();
            if (gatt == nextGatt) gatt = null;
        }

        @SuppressLint("MissingPermission")
        @Override
        public void onServicesDiscovered(BluetoothGatt nextGatt, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                emitDiagnostic("SERVICES: failed status=" + status);
                emitError("无法读取 OP Embedded BLE 服务");
                return;
            }
            BluetoothGattService service = nextGatt.getService(SERVICE_UUID);
            if (service == null) {
                emitDiagnostic("SERVICES: OP Embedded service missing");
                emitError("设备缺少 OP Embedded 传输服务");
                return;
            }
            transferCharacteristic = service.getCharacteristic(TRANSFER_UUID);
            statusCharacteristic = service.getCharacteristic(STATUS_UUID);
            if (transferCharacteristic == null || statusCharacteristic == null) {
                emitDiagnostic("SERVICES: transfer or status characteristic missing");
                emitError("设备固件不支持手机传输");
                return;
            }
            requestNextMtu(nextGatt);
        }

        @Override
        public void onMtuChanged(BluetoothGatt nextGatt, int mtu, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                negotiatedMtu = mtu;
                payloadChunkBytes = Math.max(
                        DEFAULT_PAYLOAD_CHUNK_BYTES,
                        Math.min(MAX_PAYLOAD_CHUNK_BYTES, mtu - 7));
            }
            emitDiagnostic("MTU: result=" + mtu + " status=" + status
                    + " payload=" + payloadChunkBytes);
            if ((status != BluetoothGatt.GATT_SUCCESS || mtu <= 23)
                    && mtuRequestIndex < 2) {
                requestNextMtu(nextGatt);
            } else {
                configureReady(nextGatt);
            }
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt nextGatt,
                                      BluetoothGattDescriptor descriptor,
                                      int status) {
            if (descriptor == null || !CLIENT_CONFIG_UUID.equals(descriptor.getUuid())) return;
            statusSubscriptionPending = false;
            if (status != BluetoothGatt.GATT_SUCCESS) {
                emitDiagnostic("SUBSCRIBE: failed status=" + status);
                emitError("BLE 状态订阅失败 (" + status + ")");
                return;
            }
            emitDiagnostic("SUBSCRIBE: enabled; reading initial device status");
            beginReadyStatusProbe(nextGatt);
        }

        @Override
        public void onCharacteristicRead(BluetoothGatt nextGatt,
                                         BluetoothGattCharacteristic characteristic,
                                         int status) {
            if (characteristic == null || !STATUS_UUID.equals(characteristic.getUuid())) return;
            if (initialStatusReadPending) {
                initialStatusReadPending = false;
                if (status != BluetoothGatt.GATT_SUCCESS || characteristic.getValue() == null
                        || characteristic.getValue().length < 14) {
                    emitDiagnostic("READY: initial status read failed status=" + status);
                    emitError("BLE 设备状态读取失败 (" + status + ")");
                    return;
                }
                byte[] value = characteristic.getValue();
                int received = parseReceivedOffset(value);
                int total = parseTotalBytes(value);
                if (value[2] != 0 && received > 0) {
                    emitDiagnostic("READY BLOCKED: incomplete device transfer received="
                            + received + " total=" + total);
                    emitError("设备保留了上次未完成的传输（" + received + " / " + total
                            + "）。请重启设备后重新连接；无需刷新固件。");
                    return;
                }
                linkReady = true;
                emitDiagnostic("READY: MTU=" + negotiatedMtu + " received="
                        + received + " total=" + total
                        + " receiving=" + value[2] + " complete=" + value[3]
                        + " failed=" + value[4]);
                emitEvent("connected", "OP Embedded BLE 已连接", -1, -1);
                return;
            }
            if (!uploading || !checkpointPending) return;
            statusReadPending = false;
            if (status != BluetoothGatt.GATT_SUCCESS) {
                emitDiagnostic("STATUS: read failed status=" + status);
                retryStatusRead("无法读取设备传输状态");
                return;
            }
            byte[] statusValue = characteristic.getValue();
            if (statusValue == null || statusValue.length < 14) {
                emitDiagnostic("STATUS: invalid response length="
                        + (statusValue == null ? 0 : statusValue.length));
            } else {
                emitDiagnostic("STATUS: received=" + parseReceivedOffset(statusValue)
                        + " queued=" + queuedOffset + " confirmed=" + confirmedOffset
                        + " receiving=" + statusValue[2] + " complete=" + statusValue[3]
                        + " failed=" + statusValue[4]);
            }
            handleConfirmedStatus(statusValue);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt nextGatt,
                                            BluetoothGattCharacteristic characteristic) {
            if (!uploading || characteristic != statusCharacteristic) return;
            handleStatusNotification(characteristic.getValue());
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt nextGatt,
                                          BluetoothGattCharacteristic characteristic,
                                          int status) {
            if (!uploading || characteristic == null
                    || !TRANSFER_UUID.equals(characteristic.getUuid()) || !writePending) return;
            mainHandler.removeCallbacks(writeTimeoutRunnable);
            writePending = false;
            if (status != BluetoothGatt.GATT_SUCCESS) {
                emitDiagnostic("WRITE: callback failed status=" + status
                        + " offset=" + pendingWriteOffset);
                pendingWriteLength = 0;
                if (payloadChunkBytes > FALLBACK_PAYLOAD_CHUNK_BYTES) {
                    payloadChunkBytes = FALLBACK_PAYLOAD_CHUNK_BYTES;
                    emitEvent("status", "连接较慢，已切换到兼容传输档", -1, -1);
                    mainHandler.post(uploadRunnable);
                } else {
                    stopUpload("手机蓝牙栈未能发送内容数据 (" + status + ")");
                }
                return;
            }

            queuedOffset = pendingWriteOffset + pendingWriteLength;
            pendingWriteLength = 0;
            if (queuedOffset <= payloadChunkBytes) {
                emitDiagnostic("WRITE: first packet confirmed bytes=" + queuedOffset);
            }
            if (queuedOffset - confirmedOffset >= STATUS_CHECKPOINT_BYTES
                    || queuedOffset >= uploadTotal) {
                checkpointPending = true;
                statusReadNotBefore = System.currentTimeMillis() + STATUS_READ_SETTLE_MS;
                emitDiagnostic("WRITE: checkpoint queued=" + queuedOffset
                        + " confirmed=" + confirmedOffset);
                mainHandler.postDelayed(uploadRunnable, STATUS_READ_SETTLE_MS);
            } else {
                mainHandler.post(uploadRunnable);
            }
        }

    };

    @SuppressLint("MissingPermission")
    private void requestNextMtu(BluetoothGatt nextGatt) {
        if (!connected || nextGatt == null || linkReady) return;
        int requestedMtu = mtuRequestIndex == 0 ? PRIMARY_MTU_REQUEST : FALLBACK_MTU_REQUEST;
        mtuRequestIndex += 1;
        boolean accepted = nextGatt.requestMtu(requestedMtu);
        emitDiagnostic("SERVICES: MTU " + requestedMtu + " request=" + accepted
                + " attempt=" + mtuRequestIndex);
        if (!accepted) {
            if (mtuRequestIndex < 2) {
                mainHandler.postDelayed(() -> requestNextMtu(nextGatt), 100L);
            } else {
                configureReady(nextGatt);
            }
        }
    }

    private void handleStatusNotification(byte[] value) {
        if (value == null || value.length < 14) return;
        if (value[4] != 0) {
            stopUpload("设备拒绝内容，请检查尺寸或格式");
            return;
        }
        if (value[3] != 0 && awaitingCompletion) {
            completeUpload();
            return;
        }
        int received = parseReceivedOffset(value);
        if (received > lastProgressBytes) {
            lastProgressBytes = Math.min(uploadTotal, received);
            emitEvent("progress", "设备正在接收", lastProgressBytes, uploadTotal);
        }
    }

    private int parseReceivedOffset(byte[] value) {
        return (value[5] & 0xff)
                | ((value[6] & 0xff) << 8)
                | ((value[7] & 0xff) << 16)
                | ((value[8] & 0xff) << 24);
    }

    private int parseTotalBytes(byte[] value) {
        if (value == null || value.length < 13) return 0;
        return (value[9] & 0xff)
                | ((value[10] & 0xff) << 8)
                | ((value[11] & 0xff) << 16)
                | ((value[12] & 0xff) << 24);
    }

    private void handleConfirmedStatus(byte[] value) {
        if (value == null || value.length < 14) {
            retryStatusRead("设备返回的传输状态无效");
            return;
        }
        if (value[4] != 0) {
            stopUpload("设备拒绝内容，请检查尺寸或格式");
            return;
        }
        int received = Math.max(0, Math.min(uploadTotal, parseReceivedOffset(value)));
        if (value[3] != 0) {
            completeUpload();
            return;
        }
        if (awaitingCompletion && received == uploadTotal) {
            completionReadAttempts += 1;
            if (completionReadAttempts >= 10) {
                stopUpload("设备未确认内容完成，请重新连接后重试");
                return;
            }
            statusReadNotBefore = System.currentTimeMillis() + 150L;
            mainHandler.postDelayed(uploadRunnable, 150L);
            return;
        }
        if (received < confirmedOffset) {
            retryStatusRead("设备接收位置异常");
            return;
        }
        if (received == confirmedOffset) {
            retryStatusRead("设备尚未确认新的数据");
            return;
        }

        // Only a direct read may move the replay position. Notifications are display-only.
        confirmedOffset = received;
        queuedOffset = received;
        mainHandler.removeCallbacks(writeTimeoutRunnable);
        writePending = false;
        pendingWriteLength = 0;
        checkpointPending = false;
        checkpointReadAttempts = 0;
        if (received >= lastProgressBytes + 4096 || received == uploadTotal) {
            lastProgressBytes = received;
            emitEvent("progress", "设备已接收", received, uploadTotal);
        }
        if (received == uploadTotal) {
            awaitingCompletion = true;
            checkpointPending = true;
            statusReadNotBefore = System.currentTimeMillis() + 150L;
        }
        mainHandler.post(uploadRunnable);
    }

    private void retryStatusRead(String reason) {
        checkpointReadAttempts += 1;
        emitDiagnostic("STATUS: " + reason + " attempt=" + checkpointReadAttempts
                + " queued=" + queuedOffset + " confirmed=" + confirmedOffset);
        if (checkpointReadAttempts < 3) {
            statusReadNotBefore = System.currentTimeMillis() + 120L;
            mainHandler.postDelayed(uploadRunnable, 120L);
            return;
        }
        if (payloadChunkBytes > FALLBACK_PAYLOAD_CHUNK_BYTES) {
            payloadChunkBytes = FALLBACK_PAYLOAD_CHUNK_BYTES;
            queuedOffset = confirmedOffset;
            mainHandler.removeCallbacks(writeTimeoutRunnable);
            writePending = false;
            pendingWriteLength = 0;
            checkpointPending = false;
            checkpointReadAttempts = 0;
            emitEvent("status", "连接较慢，已切换到兼容传输档", -1, -1);
            mainHandler.post(uploadRunnable);
            return;
        }
        stopUpload(reason + "，请重新连接设备后重试");
    }

    @SuppressWarnings("deprecation")
    @SuppressLint("MissingPermission")
    private void configureReady(BluetoothGatt nextGatt) {
        if (!connected || statusCharacteristic == null || linkReady
                || statusSubscriptionPending || initialStatusReadPending) return;
        boolean localNotificationEnabled = nextGatt.setCharacteristicNotification(statusCharacteristic, true);
        if (!localNotificationEnabled) {
            emitDiagnostic("SUBSCRIBE: local notification setup failed");
            emitError("BLE 状态通知初始化失败");
            return;
        }
        BluetoothGattDescriptor descriptor = statusCharacteristic.getDescriptor(CLIENT_CONFIG_UUID);
        if (descriptor == null) {
            emitDiagnostic("SUBSCRIBE: CCCD missing; probing status directly");
            beginReadyStatusProbe(nextGatt);
            return;
        }
        descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
        statusSubscriptionPending = true;
        if (!nextGatt.writeDescriptor(descriptor)) {
            statusSubscriptionPending = false;
            emitDiagnostic("SUBSCRIBE: descriptor write rejected");
            emitError("BLE 状态订阅请求被系统拒绝");
            return;
        }
        emitDiagnostic("SUBSCRIBE: enabling status notifications");
    }

    @SuppressWarnings("deprecation")
    @SuppressLint("MissingPermission")
    private void beginReadyStatusProbe(BluetoothGatt nextGatt) {
        if (initialStatusReadPending || statusCharacteristic == null) return;
        initialStatusReadPending = true;
        if (!nextGatt.readCharacteristic(statusCharacteristic)) {
            initialStatusReadPending = false;
            emitDiagnostic("READY: initial status read request rejected");
            emitError("BLE 初始状态读取请求被系统拒绝");
            return;
        }
        emitDiagnostic("READY: reading device status");
    }

    @SuppressLint("MissingPermission")
    private void disconnectBle() {
        stopScan();
        if (gatt != null) gatt.disconnect();
    }

    private static int contentCapacityForProfile(String profileId) {
        if ("co5300_waveshare_amoled_1_75c".equals(profileId)) return WAVESHARE_CONTENT_BYTES;
        if ("co5300_m5stack_stopwatch".equals(profileId)
                || "ili9342_m5stack_cores3".equals(profileId)) return LARGE_FLASH_CONTENT_BYTES;
        return 0;
    }

    private static int profileWidth(String profileId) {
        return "ili9342_m5stack_cores3".equals(profileId) ? 320 : 466;
    }

    private static int profileHeight(String profileId) {
        return "ili9342_m5stack_cores3".equals(profileId) ? 240 : 466;
    }

    private static String formatMiB(int bytes) {
        return String.format(java.util.Locale.US, "%.2f", bytes / 1024.0 / 1024.0);
    }

    private String setDeviceProfile(String profileId) {
        if (contentCapacityForProfile(profileId) == 0) {
            return "不支持的屏幕方案，请重新选择";
        }
        selectedProfileId = profileId;
        return "";
    }

    private void beginPayload(int totalBytes) throws IOException {
        closePayloadOutput();
        int capacity = contentCapacityForProfile(selectedProfileId);
        if (totalBytes <= 24 || capacity == 0 || totalBytes > capacity) {
            throw new IOException("内容大小必须在 24 字节至 " + formatMiB(capacity) + " MiB 之间（当前屏幕方案）");
        }
        payloadFile = new File(getCacheDir(), "openpencil-content.bin");
        payloadOutput = new FileOutputStream(payloadFile, false);
        payloadExpectedBytes = totalBytes;
        payloadWrittenBytes = 0;
    }

    private void appendPayload(String encoded) throws IOException {
        if (payloadOutput == null) throw new IOException("尚未开始准备内容");
        byte[] bytes = Base64.decode(encoded, Base64.DEFAULT);
        if (payloadWrittenBytes + bytes.length > payloadExpectedBytes) {
            throw new IOException("内容数据超过声明长度");
        }
        payloadOutput.write(bytes);
        payloadWrittenBytes += bytes.length;
    }

    private void finishPayload() throws IOException {
        closePayloadOutput();
        if (payloadWrittenBytes != payloadExpectedBytes) {
            throw new IOException("内容写入不完整：" + payloadWrittenBytes + " / " + payloadExpectedBytes);
        }
        emitEvent("prepared", "内容已准备，可以上传", payloadWrittenBytes, payloadExpectedBytes);
    }

    private void closePayloadOutput() throws IOException {
        if (payloadOutput == null) return;
        payloadOutput.flush();
        payloadOutput.close();
        payloadOutput = null;
    }

    private void startUpload() {
        if (!connected || !linkReady || gatt == null || transferCharacteristic == null) {
            emitError("请先连接 OP Embedded BLE");
            return;
        }
        if (payloadFile == null || !payloadFile.isFile() || payloadWrittenBytes != payloadExpectedBytes) {
            emitError("请先选择并处理图片");
            return;
        }
        stopUpload(null);
        try {
            uploadInput = new RandomAccessFile(payloadFile, "r");
            queuedOffset = 0;
            confirmedOffset = 0;
            uploadTotal = payloadExpectedBytes;
            lastProgressBytes = 0;
            uploadStartedAt = System.currentTimeMillis();
            uploading = true;
            checkpointReadAttempts = 0;
            completionReadAttempts = 0;
            checkpointPending = false;
            statusReadPending = false;
            awaitingCompletion = false;
            writePending = false;
            pendingWriteOffset = 0;
            pendingWriteLength = 0;
            transferCharacteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
            emitDiagnostic("UPLOAD: start MTU=" + negotiatedMtu + " payload=" + payloadChunkBytes
                    + " total=" + uploadTotal);
            emitEvent("status", "开始通过 BLE 上传…", 0, uploadTotal);
            mainHandler.post(uploadRunnable);
        } catch (IOException error) {
            emitError(error.getMessage());
        }
    }

    private void requestUsbContentUpload() {
        if (usbContentUploading) return;
        if (payloadFile == null || !payloadFile.isFile() || payloadWrittenBytes != payloadExpectedBytes) {
            emitError("请先选择并处理图片");
            return;
        }
        UsbDevice device = EspUsbFlasher.findDevice(usbManager);
        if (device == null) {
            emitError("未发现 ESP32 USB 设备，请连接 USB OTG");
            return;
        }
        disconnectBle();
        pendingFirmwareMode = "content";
        if (!usbManager.hasPermission(device)) {
            PendingIntent permissionIntent = PendingIntent.getBroadcast(
                    this,
                    1402,
                    new Intent(USB_PERMISSION_ACTION).setPackage(getPackageName()),
                    Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
            emitEvent("status", "等待 USB 设备授权…", -1, -1);
            usbManager.requestPermission(device, permissionIntent);
            return;
        }
        pendingFirmwareMode = null;
        startUsbContentUpload(device);
    }

    private void startUsbContentUpload(UsbDevice device) {
        if (usbContentUploading) return;
        usbContentUploading = true;
        emitEvent("status", "正在打开 USB 内容接口…", 0, payloadExpectedBytes);
        new Thread(() -> {
            try (RandomAccessFile input = new RandomAccessFile(payloadFile, "r");
                 EspUsbContentUploader uploader = EspUsbContentUploader.open(usbManager, device)) {
                uploader.upload(input, payloadExpectedBytes, profileWidth(selectedProfileId),
                        profileHeight(selectedProfileId), new EspUsbContentUploader.ProgressListener() {
                            @Override
                            public void onProgress(int written, int total, String message) {
                                emitEvent("progress", message, written, total);
                            }

                            @Override
                            public void onLog(String message) {
                                emitEvent("status", message, -1, -1);
                            }
                        });
                usbContentUploading = false;
                emitEvent("complete", "USB 内容上传完成，设备正在重启", payloadExpectedBytes, payloadExpectedBytes);
            } catch (Exception error) {
                usbContentUploading = false;
                emitError(error.getMessage() == null ? "USB 内容上传失败" : error.getMessage());
            }
        }, "op-usb-content-upload").start();
    }

    private final Runnable uploadRunnable = new Runnable() {
        @SuppressWarnings("deprecation")
        @SuppressLint("MissingPermission")
        @Override
        public void run() {
            if (!uploading || uploadInput == null || gatt == null || transferCharacteristic == null) return;
            if (!connected) {
                stopUpload("上传过程中 BLE 已断开");
                return;
            }
            if (writePending) return;
            if (checkpointPending) {
                if (System.currentTimeMillis() < statusReadNotBefore) {
                    mainHandler.postDelayed(this, 20L);
                    return;
                }
                if (statusReadPending) {
                    if (System.currentTimeMillis() >= statusReadDeadline) {
                        statusReadPending = false;
                        retryStatusRead("设备传输状态读取超时");
                    } else {
                        mainHandler.postDelayed(this, 40L);
                    }
                    return;
                }
                statusReadPending = true;
                emitDiagnostic("STATUS: requesting checkpoint queued=" + queuedOffset
                        + " confirmed=" + confirmedOffset);
                if (!gatt.readCharacteristic(statusCharacteristic)) {
                    statusReadPending = false;
                    retryStatusRead("无法请求设备传输状态");
                } else {
                    statusReadDeadline = System.currentTimeMillis() + 900L;
                    mainHandler.postDelayed(this, 40L);
                }
                return;
            }
            try {
                if (queuedOffset >= uploadTotal) {
                    checkpointPending = true;
                    statusReadNotBefore = System.currentTimeMillis() + 150L;
                    mainHandler.postDelayed(this, 150L);
                    return;
                }

                int length = Math.min(payloadChunkBytes, uploadTotal - queuedOffset);
                byte[] content = new byte[length];
                uploadInput.seek(queuedOffset);
                uploadInput.readFully(content);
                byte[] packet = new byte[length + 4];
                ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN).putInt(queuedOffset).put(content);
                transferCharacteristic.setValue(packet);
                pendingWriteOffset = queuedOffset;
                pendingWriteLength = length;
                writePending = true;
                if (!gatt.writeCharacteristic(transferCharacteristic)) {
                    writePending = false;
                    pendingWriteLength = 0;
                    emitDiagnostic("WRITE: rejected offset=" + queuedOffset);
                    mainHandler.postDelayed(this, 4L);
                    return;
                }
                mainHandler.postDelayed(writeTimeoutRunnable, WRITE_CALLBACK_TIMEOUT_MS);
            } catch (IOException error) {
                stopUpload(error.getMessage());
            }
        }
    };

    private final Runnable writeTimeoutRunnable = () -> {
        if (!uploading || !writePending) return;
        emitDiagnostic("WRITE: callback timeout offset=" + pendingWriteOffset);
        writePending = false;
        pendingWriteLength = 0;
        checkpointPending = true;
        statusReadNotBefore = System.currentTimeMillis() + 120L;
        mainHandler.postDelayed(uploadRunnable, 120L);
    };

    private void stopUpload(String errorMessage) {
        uploading = false;
        checkpointPending = false;
        statusReadPending = false;
        awaitingCompletion = false;
        writePending = false;
        pendingWriteLength = 0;
        mainHandler.removeCallbacks(uploadRunnable);
        mainHandler.removeCallbacks(writeTimeoutRunnable);
        if (uploadInput != null) {
            try {
                uploadInput.close();
            } catch (IOException ignored) {
            }
            uploadInput = null;
        }
        if (errorMessage != null) emitError(errorMessage);
    }

    private void emitError(String message) {
        emitEvent("error", message == null ? "未知错误" : message, -1, -1);
    }

    private void completeUpload() {
        emitUploadRate();
        stopUpload(null);
        emitEvent("complete", CONTENT_COMPLETE_MESSAGE, uploadTotal, uploadTotal);
    }

    private void emitUploadRate() {
        if (uploadStartedAt <= 0 || uploadTotal <= 0) return;
        long elapsedMs = Math.max(1L, System.currentTimeMillis() - uploadStartedAt);
        long kibPerSecond = Math.round(uploadTotal * 1000.0 / elapsedMs / 1024.0);
        emitDiagnostic("UPLOAD: complete bytes=" + uploadTotal
                + " elapsed=" + elapsedMs + "ms rate=" + kibPerSecond + "KiB/s"
                + " mtu=" + negotiatedMtu + " payload=" + payloadChunkBytes);
    }

    private void emitDiagnostic(String message) {
        emitEvent("diagnostic", message, -1, -1);
    }

    private void flashFirmware(String mode) {
        if (firmwareFlashing) return;
        if (!"usb".equals(mode) && !"ble".equals(mode)) {
            emitFirmwareEvent("firmware-error", "未知固件版本");
            return;
        }
        if (usbManager == null) {
            emitFirmwareEvent("firmware-error", "当前手机不支持 USB Host");
            return;
        }
        UsbDevice device = EspUsbFlasher.findDevice(usbManager);
        if (device == null) {
            emitFirmwareEvent("firmware-error", "未发现 ESP32 USB 设备，请使用 USB OTG 连接并进入下载模式");
            return;
        }
        disconnectBle();
        pendingFirmwareMode = mode;
        if (!usbManager.hasPermission(device)) {
            PendingIntent permissionIntent = PendingIntent.getBroadcast(
                    this,
                    1401,
                    new Intent(USB_PERMISSION_ACTION).setPackage(getPackageName()),
                    Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
            emitFirmwareEvent("firmware-status", "等待 USB 设备授权…");
            usbManager.requestPermission(device, permissionIntent);
            return;
        }
        pendingFirmwareMode = null;
        startFirmwareFlash(device, mode);
    }

    private void startFirmwareFlash(UsbDevice device, String mode) {
        if (firmwareFlashing) return;
        firmwareFlashing = true;
        emitFirmwareEvent("firmware-status", "正在打开 USB 下载接口…");
        new Thread(() -> {
            try {
                String root = ("ble".equals(mode) ? "ble-frame" : "usb-frame")
                        + "/" + selectedProfileId + "/";
                List<EspUsbFlasher.Segment> segments = new ArrayList<>();
                segments.add(new EspUsbFlasher.Segment(0x0000, readAsset(root + "bootloader.bin")));
                segments.add(new EspUsbFlasher.Segment(0x8000, readAsset(root + "partition-table.bin")));
                segments.add(new EspUsbFlasher.Segment(0x10000, readAsset(root + "st7789_simple.bin")));
                segments.add(new EspUsbFlasher.Segment(0x310000, readAsset(root + "content-reset.bin")));
                try (EspUsbFlasher flasher = EspUsbFlasher.open(usbManager, device, getAssets())) {
                    flasher.flash(segments, new EspUsbFlasher.ProgressListener() {
                        @Override
                        public void onProgress(int written, int total, String message) {
                            emitFirmwareEvent("firmware-progress", message, written, total);
                        }

                        @Override
                        public void onLog(String message) {
                            emitFirmwareEvent("firmware-log", message);
                        }
                    });
                }
                firmwareFlashing = false;
                emitFirmwareEvent("firmware-complete", "ble".equals(mode)
                        ? "BLE 固件烧录完成，设备正在重启"
                        : "USB 固件烧录完成，设备正在重启");
            } catch (Exception error) {
                firmwareFlashing = false;
                String message = error.getMessage();
                emitFirmwareEvent("firmware-error", message == null ? "固件烧录失败" : message);
            }
        }, "op-firmware-flash").start();
    }

    private byte[] readAsset(String path) throws IOException {
        try (InputStream input = getAssets().open(path);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            return output.toByteArray();
        }
    }

    private void emitFirmwareEvent(String type, String message) {
        emitFirmwareEvent(type, message, -1, -1);
    }

    private void emitFirmwareEvent(String type, String message, int written, int total) {
        JSONObject object = new JSONObject();
        try {
            object.put("type", type);
            object.put("message", message == null ? "" : message);
            if (written >= 0) object.put("written", written);
            if (total >= 0) object.put("total", total);
        } catch (JSONException ignored) {
        }
        String script = "window.OpenPencilApp&&window.OpenPencilApp.nativeEvent(" + object + ")";
        runOnUiThread(() -> webView.evaluateJavascript(script, null));
    }

    private void emitEvent(String type, String message, int written, int total) {
        JSONObject object = new JSONObject();
        try {
            object.put("type", type);
            object.put("message", message);
            if (written >= 0) object.put("written", written);
            if (total >= 0) object.put("total", total);
        } catch (JSONException ignored) {
        }
        String script = "window.OpenPencilApp&&window.OpenPencilApp.nativeEvent(" + object + ")";
        runOnUiThread(() -> webView.evaluateJavascript(script, null));
    }

    private static final int REQUEST_CAMERA_CAPTURE = 1201;
    private static final int REQUEST_PICK_MEDIA = 1202;

    private void requestCamera() {
        File captureFile = CameraFileProvider.captureFile(this);
        if (captureFile.exists() && !captureFile.delete()) {
            emitError("无法准备拍照文件");
            return;
        }
        cameraCaptureUri = CameraFileProvider.captureUri(this);
        Intent intent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        intent.putExtra(MediaStore.EXTRA_OUTPUT, cameraCaptureUri);
        intent.setClipData(ClipData.newRawUri("OP Embedded photo", cameraCaptureUri));
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if (intent.resolveActivity(getPackageManager()) == null) {
            cameraCaptureUri = null;
            emitError("没有找到可用的相机应用");
            return;
        }
        startActivityForResult(intent, REQUEST_CAMERA_CAPTURE);
    }

    private void openNativeMediaPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"image/*", "video/*"});
        startActivityForResult(intent, REQUEST_PICK_MEDIA);
    }

    private void importMedia(Uri uri, String mimeType) {
        if (uri == null) return;
        emitEvent("status", "正在读取媒体…", -1, -1);
        new Thread(() -> {
            String normalizedType = normalizeMediaMime(mimeType, uri);
            String name = createMediaName(normalizedType);
            File target = new File(getCacheDir(), name);
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IOException("无法读取所选媒体");
                try (FileOutputStream output = new FileOutputStream(target, false)) {
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
                }
                runOnUiThread(() -> deliverManagedMedia(name, normalizedType));
            } catch (Exception error) {
                target.delete();
                String message = error.getMessage();
                runOnUiThread(() -> emitError(message == null ? "媒体读取失败" : message));
            }
        }, "op-media-import").start();
    }

    private void deliverManagedMedia(String name, String mimeType) {
        File file = new File(getCacheDir(), name);
        if (!file.isFile() || file.length() == 0) {
            emitError("媒体文件为空或不存在");
            return;
        }
        String normalizedMime = normalizeMediaMime(mimeType, null);
        String url = "https://appassets.androidplatform.net/media/" + name;
        webView.evaluateJavascript("window.OpenPencilApp.nativeMedia(" + JSONObject.quote(url)
                + "," + JSONObject.quote(normalizedMime) + ")", null);
    }

    private String normalizeMediaMime(String mimeType, Uri sourceUri) {
        if (mimeType != null && (mimeType.startsWith("video/") || mimeType.startsWith("image/"))) {
            return mimeType;
        }
        String path = sourceUri == null ? "" : String.valueOf(sourceUri.getLastPathSegment()).toLowerCase();
        if (path.endsWith(".mov")) return "video/quicktime";
        if (path.endsWith(".webm")) return "video/webm";
        if (path.endsWith(".mp4") || path.endsWith(".m4v")) return "video/mp4";
        if (path.endsWith(".png")) return "image/png";
        if (path.endsWith(".webp")) return "image/webp";
        return "image/jpeg";
    }

    private String createMediaName(String mimeType) {
        String extension = ".jpg";
        if ("video/quicktime".equals(mimeType)) extension = ".mov";
        else if ("video/webm".equals(mimeType)) extension = ".webm";
        else if (mimeType.startsWith("video/")) extension = ".mp4";
        else if ("image/png".equals(mimeType)) extension = ".png";
        else if ("image/webp".equals(mimeType)) extension = ".webp";
        return "openpencil-media-" + Long.toUnsignedString(System.nanoTime()) + extension;
    }

    private final class NativeBridge {
        @JavascriptInterface
        public void capturePhoto() { runOnUiThread(MainActivity.this::requestCamera); }

        @JavascriptInterface
        public void pickMedia() { runOnUiThread(MainActivity.this::openNativeMediaPicker); }
        @JavascriptInterface
        public String setDeviceProfile(String profileId) {
            return MainActivity.this.setDeviceProfile(profileId);
        }
        @JavascriptInterface
        public void connect() {
            runOnUiThread(() -> {
                if (hasBlePermissions()) startBleScan();
                else requestBlePermissions();
            });
        }

        @JavascriptInterface
        public void disconnect() {
            runOnUiThread(MainActivity.this::disconnectBle);
        }

        @JavascriptInterface
        public void openAppSettings() {
            Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
            intent.setData(Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        }

        @JavascriptInterface
        public String beginPayload(int totalBytes) {
            try {
                MainActivity.this.beginPayload(totalBytes);
                return "";
            } catch (IOException error) {
                return error.getMessage();
            }
        }

        @JavascriptInterface
        public String appendPayloadChunk(String encoded) {
            try {
                appendPayload(encoded);
                return "";
            } catch (IOException error) {
                return error.getMessage();
            }
        }

        @JavascriptInterface
        public String finishPayload() {
            try {
                MainActivity.this.finishPayload();
                return "";
            } catch (IOException error) {
                return error.getMessage();
            }
        }

        @JavascriptInterface
        public void upload() {
            runOnUiThread(MainActivity.this::startUpload);
        }

        @JavascriptInterface
        public void uploadUsb() {
            runOnUiThread(MainActivity.this::requestUsbContentUpload);
        }

        @JavascriptInterface
        public void flashFirmware(String mode) {
            runOnUiThread(() -> MainActivity.this.flashFirmware(mode));
        }
    }

    @Override
    protected void onDestroy() {
        stopUpload(null);
        try {
            closePayloadOutput();
        } catch (IOException ignored) {
        }
        disconnectBle();
        try {
            unregisterReceiver(usbPermissionReceiver);
        } catch (IllegalArgumentException ignored) {
        }
        if (webView != null) webView.destroy();
        super.onDestroy();
    }
}
