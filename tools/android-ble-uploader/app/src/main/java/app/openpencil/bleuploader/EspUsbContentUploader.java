package app.openpencil.bleuploader;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.zip.Deflater;

/** OPUSB/1 content transport over the ESP32-S3 USB Serial/JTAG interface. */
final class EspUsbContentUploader implements AutoCloseable {
    interface ProgressListener {
        void onProgress(int written, int total, String message);
        default void onLog(String message) { }
    }

    private static final String PREFIX = "OPUSB/1";
    private static final int HEADER_BYTES = 24;
    private static final int CHUNK_BYTES = 0x10000;
    private static final int SERVICE_VERSION = 6;
    private static final int USB_TIMEOUT_MS = 2000;
    private static final int COMMAND_TIMEOUT_MS = 15000;

    private final UsbDeviceConnection connection;
    private final UsbInterface dataInterface;
    private final UsbEndpoint input;
    private final UsbEndpoint output;
    private final int controlInterfaceId;
    private byte[] pendingInput = new byte[0];
    private int pendingInputOffset;

    private EspUsbContentUploader(UsbDeviceConnection connection,
                                  UsbInterface dataInterface,
                                  UsbEndpoint input,
                                  UsbEndpoint output,
                                  int controlInterfaceId) {
        this.connection = connection;
        this.dataInterface = dataInterface;
        this.input = input;
        this.output = output;
        this.controlInterfaceId = controlInterfaceId;
    }

    static EspUsbContentUploader open(UsbManager manager, UsbDevice device) throws IOException {
        if (manager == null || device == null) throw new IOException("没有可用的 USB 设备");
        UsbDeviceConnection connection = manager.openDevice(device);
        if (connection == null) throw new IOException("无法打开 USB 设备，请检查 OTG 权限");

        UsbInterface selected = null;
        UsbEndpoint input = null;
        UsbEndpoint output = null;
        int controlInterfaceId = 0;
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface candidate = device.getInterface(i);
            if (candidate.getInterfaceClass() == UsbConstants.USB_CLASS_COMM) {
                controlInterfaceId = candidate.getId();
            }
            UsbEndpoint candidateInput = null;
            UsbEndpoint candidateOutput = null;
            for (int j = 0; j < candidate.getEndpointCount(); j++) {
                UsbEndpoint endpoint = candidate.getEndpoint(j);
                if (endpoint.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
                if (endpoint.getDirection() == UsbConstants.USB_DIR_IN) candidateInput = endpoint;
                if (endpoint.getDirection() == UsbConstants.USB_DIR_OUT) candidateOutput = endpoint;
            }
            if (candidateInput != null && candidateOutput != null && selected == null) {
                selected = candidate;
                input = candidateInput;
                output = candidateOutput;
            }
        }
        if (selected == null || input == null || output == null) {
            connection.close();
            throw new IOException("USB 设备没有可用的双向 Bulk 接口");
        }
        if (!connection.claimInterface(selected, true)) {
            connection.close();
            throw new IOException("无法占用 USB 内容接口");
        }
        return new EspUsbContentUploader(connection, selected, input, output, controlInterfaceId);
    }

    void upload(RandomAccessFile content,
                int totalBytes,
                int width,
                int height,
                ProgressListener listener) throws IOException {
        if (content == null || totalBytes < HEADER_BYTES) throw new IOException("内容数据不完整");
        setLineState(false, false);
        log(listener, "正在连接 USB 内容服务…");
        writeLine("HELLO");
        String ready = readProtocolLine(COMMAND_TIMEOUT_MS);
        String[] fields = ready.split(" ");
        if (fields.length < 6 || !PREFIX.equals(fields[0]) || !"READY".equals(fields[1])) {
            throw new IOException("USB 内容服务握手失败：" + ready);
        }
        int version = parseInt(fields[2], "USB 服务版本");
        int deviceWidth = parseInt(fields[3], "USB 屏幕宽度");
        int deviceHeight = parseInt(fields[4], "USB 屏幕高度");
        int capacity = parseInt(fields[5], "USB 内容容量");
        int firmwareMode = fields.length >= 7 ? parseInt(fields[6], "USB 固件模式") : -1;
        if (version != SERVICE_VERSION) throw new IOException("USB 内容服务版本不匹配：" + version);
        if (deviceWidth != width || deviceHeight != height) {
            throw new IOException(String.format("设备分辨率为 %d × %d，与当前方案不匹配", deviceWidth, deviceHeight));
        }
        if (firmwareMode >= 0 && firmwareMode != 2) throw new IOException("设备不是 USB 内容固件");
        if (totalBytes > capacity) throw new IOException("内容超过 USB 分区容量");
        log(listener, String.format("USB 内容服务已连接，容量 %.2f MiB", capacity / 1024.0 / 1024.0));

        content.seek(0);
        byte[] header = new byte[HEADER_BYTES];
        content.readFully(header);
        writeLine("BEGIN " + totalBytes);
        writeAll(header);
        expect("ACK 0");

        byte[] raw = new byte[CHUNK_BYTES];
        int offset = 0;
        while (offset < totalBytes - HEADER_BYTES) {
            int length = Math.min(CHUNK_BYTES, totalBytes - HEADER_BYTES - offset);
            content.seek(HEADER_BYTES + offset);
            content.readFully(raw, 0, length);
            byte[] compressed = deflate(raw, length);
            boolean useCompression = compressed.length < length;
            byte[] encoded = useCompression ? compressed : Arrays.copyOf(raw, length);
            writeLine(String.format("CHUNK %d %d %d %d", offset, length, encoded.length,
                    useCompression ? 1 : 0));
            writeAll(encoded);
            expect("ACK " + (offset + length));
            offset += length;
            if (listener != null) {
                listener.onProgress(HEADER_BYTES + offset, totalBytes, "正在通过 USB 上传");
            }
        }
        writeLine("END");
        expect("DONE");
        if (listener != null) listener.onProgress(totalBytes, totalBytes, "USB 内容上传完成，设备正在重启");
    }

    private byte[] deflate(byte[] data, int length) throws IOException {
        Deflater compressor = new Deflater(9);
        compressor.setInput(data, 0, length);
        compressor.finish();
        ByteArrayOutputStream result = new ByteArrayOutputStream(length);
        byte[] buffer = new byte[8192];
        try {
            while (!compressor.finished()) {
                int count = compressor.deflate(buffer);
                if (count == 0 && !compressor.finished()) throw new IOException("USB 内容压缩失败");
                result.write(buffer, 0, count);
            }
            return result.toByteArray();
        } finally {
            compressor.end();
        }
    }

    private void expect(String expected) throws IOException {
        String line = readProtocolLine(COMMAND_TIMEOUT_MS);
        if (line.startsWith(PREFIX + " ERR ")) throw new IOException("USB 设备拒绝内容：" + line);
        if (!line.equals(PREFIX + " " + expected)) throw new IOException("USB 响应异常：" + line);
    }

    private String readProtocolLine(int timeoutMs) throws IOException {
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            byte[] buffer = new byte[512];
            int remaining = (int) Math.max(1, Math.min(USB_TIMEOUT_MS, deadline - System.currentTimeMillis()));
            int count = readUsb(buffer, buffer.length, remaining);
            if (count <= 0) continue;
            for (int i = 0; i < count; i++) {
                if (buffer[i] == '\n') {
                    String value = new String(line.toByteArray(), StandardCharsets.UTF_8).replace("\r", "");
                    if (value.startsWith(PREFIX)) {
                        if (i + 1 < count) {
                            pendingInput = Arrays.copyOfRange(buffer, i + 1, count);
                            pendingInputOffset = 0;
                        }
                        return value;
                    }
                    line.reset();
                } else {
                    line.write(buffer[i]);
                    if (line.size() > 512) line.reset();
                }
            }
        }
        throw new IOException("等待 USB 设备响应超时");
    }

    private void writeLine(String line) throws IOException {
        writeAll((PREFIX + " " + line + "\n").getBytes(StandardCharsets.UTF_8));
    }

    private void writeAll(byte[] data) throws IOException {
        int offset = 0;
        while (offset < data.length) {
            int count = connection.bulkTransfer(output, data, offset, data.length - offset, USB_TIMEOUT_MS);
            if (count <= 0) throw new IOException("USB 内容写入失败");
            offset += count;
        }
    }

    private int readUsb(byte[] buffer, int length, int timeoutMs) {
        int copied = 0;
        int pending = pendingInput.length - pendingInputOffset;
        if (pending > 0) {
            copied = Math.min(length, pending);
            System.arraycopy(pendingInput, pendingInputOffset, buffer, 0, copied);
            pendingInputOffset += copied;
            if (pendingInputOffset >= pendingInput.length) {
                pendingInput = new byte[0];
                pendingInputOffset = 0;
            }
            if (copied == length) return copied;
        }
        int count = connection.bulkTransfer(input, buffer, copied, length - copied, timeoutMs);
        return copied + Math.max(0, count);
    }

    private void setLineState(boolean dtr, boolean rts) {
        int value = (dtr ? 1 : 0) | (rts ? 2 : 0);
        connection.controlTransfer(0x21, 0x22, value, controlInterfaceId, null, 0, 100);
    }

    private static int parseInt(String value, String name) throws IOException {
        try {
            return Integer.parseInt(value);
        } catch (NumberFormatException error) {
            throw new IOException(name + "无效");
        }
    }

    private static void log(ProgressListener listener, String message) {
        if (listener != null) listener.onLog(message);
    }

    @Override
    public void close() {
        connection.releaseInterface(dataInterface);
        connection.close();
    }
}
