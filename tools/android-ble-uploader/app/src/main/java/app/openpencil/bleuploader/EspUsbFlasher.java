package app.openpencil.bleuploader;

import android.content.res.AssetManager;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.util.Base64;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.Deflater;

/** Minimal ESP serial bootloader client for USB OTG firmware flashing. */
final class EspUsbFlasher implements AutoCloseable {
    interface ProgressListener {
        void onProgress(int written, int total, String message);

        default void onLog(String message) {
        }
    }

    static final class Segment {
        final int offset;
        final byte[] data;

        Segment(int offset, byte[] data) {
            this.offset = offset;
            this.data = data;
        }
    }

    private static final int ESP_FLASH_BEGIN = 0x02;
    private static final int ESP_FLASH_DATA = 0x03;
    private static final int ESP_FLASH_END = 0x04;
    private static final int ESP_FLASH_DEFL_BEGIN = 0x10;
    private static final int ESP_FLASH_DEFL_DATA = 0x11;
    private static final int ESP_FLASH_DEFL_END = 0x12;
    private static final int ESP_MEM_BEGIN = 0x05;
    private static final int ESP_MEM_END = 0x06;
    private static final int ESP_MEM_DATA = 0x07;
    private static final int ESP_SYNC = 0x08;
    private static final int ESP_CHECKSUM_MAGIC = 0xEF;
    // ESP32-S3 ROM (and esptool-js' ESP32S3ROM target) uses 0x400.
    private static final int BLOCK_SIZE = 0x400;
    private static final int RAM_BLOCK_SIZE = 0x1800;
    private static final int USB_TIMEOUT_MS = 1500;
    private static final int COMMAND_TIMEOUT_MS = 10000;

    private final UsbDeviceConnection connection;
    private final UsbInterface dataInterface;
    private final UsbEndpoint input;
    private final UsbEndpoint output;
    private final int controlInterfaceId;
    private final int vendorId;
    private final int productId;
    private final AssetManager assets;
    private byte[] pendingInput = new byte[0];
    private int pendingInputOffset;
    private boolean stubRunning;

    private EspUsbFlasher(UsbDeviceConnection connection,
                          UsbInterface dataInterface,
                          UsbEndpoint input,
                          UsbEndpoint output,
                          int controlInterfaceId,
                          int vendorId,
                          int productId,
                          AssetManager assets) {
        this.connection = connection;
        this.dataInterface = dataInterface;
        this.input = input;
        this.output = output;
        this.controlInterfaceId = controlInterfaceId;
        this.vendorId = vendorId;
        this.productId = productId;
        this.assets = assets;
    }

    static boolean isEspressif(UsbDevice device) {
        return device != null && (device.getVendorId() == 0x303A || hasBulkPair(device));
    }

    static UsbDevice findDevice(UsbManager manager) {
        if (manager == null) return null;
        UsbDevice fallback = null;
        for (UsbDevice device : manager.getDeviceList().values()) {
            if (!hasBulkPair(device)) continue;
            if (device.getVendorId() == 0x303A) return device;
            if (fallback == null) fallback = device;
        }
        return fallback;
    }

    private static boolean hasBulkPair(UsbDevice device) {
        if (device == null) return false;
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface usbInterface = device.getInterface(i);
            boolean hasInput = false;
            boolean hasOutput = false;
            for (int j = 0; j < usbInterface.getEndpointCount(); j++) {
                UsbEndpoint endpoint = usbInterface.getEndpoint(j);
                if (endpoint.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
                if (endpoint.getDirection() == UsbConstants.USB_DIR_IN) hasInput = true;
                if (endpoint.getDirection() == UsbConstants.USB_DIR_OUT) hasOutput = true;
            }
            if (hasInput && hasOutput) return true;
        }
        return false;
    }

    static EspUsbFlasher open(UsbManager manager, UsbDevice device) throws IOException {
        return open(manager, device, null);
    }

    static EspUsbFlasher open(UsbManager manager,
                              UsbDevice device,
                              AssetManager assets) throws IOException {
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
            throw new IOException("无法占用 USB 串口接口");
        }
        return new EspUsbFlasher(connection,
                selected,
                input,
                output,
                controlInterfaceId,
                device.getVendorId(),
                device.getProductId(),
                assets);
    }

    void flash(List<Segment> segments, ProgressListener listener) throws IOException {
        if (segments == null || segments.isEmpty()) throw new IOException("固件分段为空");
        log(listener, "USB " + describeUsb() + "，开始 USB Serial/JTAG 复位");
        resetToBootloader(listener);
        log(listener, "复位完成，开始同步 ESP32 ROM Bootloader");
        sync(listener);
        uploadStub(listener);

        int total = 0;
        for (Segment segment : segments) total += segment.data.length;
        int written = 0;
        for (Segment segment : segments) {
            log(listener, String.format("写入分段 offset=0x%06X bytes=%d", segment.offset, segment.data.length));
            flashSegment(segment, listener, written, total);
            written += segment.data.length;
        }
        // esptool-js resets the stub through its hard-reset path after writing.
        resetToApplication(listener);
        if (listener != null) listener.onProgress(total, total, "固件写入完成，设备正在重启");
    }

    private void flashSegment(Segment segment,
                              ProgressListener listener,
                              int writtenBefore,
                              int total) throws IOException {
        byte[] image = padToFour(segment.data);
        byte[] compressed = deflate(image);
        int eraseSize = stubRunning ? image.length : align(image.length, BLOCK_SIZE);
        int blocks = (compressed.length + BLOCK_SIZE - 1) / BLOCK_SIZE;
        // Stub mode uses the four standard fields. ROM mode additionally
        // carries encrypted_write, matching esptool-js' S3 path.
        ByteBuffer begin = ByteBuffer.allocate(stubRunning ? 16 : 20)
                .order(ByteOrder.LITTLE_ENDIAN);
        begin.putInt(eraseSize)
                .putInt(blocks)
                .putInt(BLOCK_SIZE)
                .putInt(segment.offset);
        if (!stubRunning) begin.putInt(0); // encrypted_write = false
        command(ESP_FLASH_DEFL_BEGIN, begin.array(), COMMAND_TIMEOUT_MS);
        log(listener, String.format("压缩分段 %d -> %d bytes", image.length, compressed.length));

        int sequence = 0;
        int offset = 0;
        while (offset < compressed.length) {
            int length = Math.min(BLOCK_SIZE, compressed.length - offset);
            ByteBuffer body = ByteBuffer.allocate(16 + length).order(ByteOrder.LITTLE_ENDIAN);
            body.putInt(length).putInt(sequence++).putInt(0).putInt(0);
            body.put(compressed, offset, length);
            commandWithChecksum(ESP_FLASH_DEFL_DATA, body.array(), compressed, offset, length, COMMAND_TIMEOUT_MS);
            offset += length;
            int uncompressedWritten = Math.min(image.length,
                    (int) (((long) offset * image.length) / compressed.length));
            if (listener != null) {
                listener.onProgress(writtenBefore + uncompressedWritten, total, "正在写入固件");
            }
        }
        if (stubRunning) {
            command(ESP_FLASH_DEFL_END, littleEndianInt(1), COMMAND_TIMEOUT_MS);
        }
        log(listener, String.format("分段 0x%06X 写入完成", segment.offset));
    }

    private void uploadStub(ProgressListener listener) throws IOException {
        if (assets == null) {
            throw new IOException("缺少 ESP32-S3 Web 烧录 stub");
        }
        try {
            JSONObject json;
            try (InputStream input = assets.open("stub_flasher_32s3.json")) {
                json = new JSONObject(new String(readAll(input), "UTF-8"));
            }
            byte[] text = Base64.decode(json.getString("text"), Base64.DEFAULT);
            byte[] data = Base64.decode(json.getString("data"), Base64.DEFAULT);
            int entry = json.getInt("entry");
            uploadStubPart(text, json.getInt("text_start"), listener);
            uploadStubPart(data, json.getInt("data_start"), listener);
            ByteBuffer finish = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
            finish.putInt(0).putInt(entry);
            command(ESP_MEM_END, finish.array(), COMMAND_TIMEOUT_MS);
            // esptool-js reads the stub greeting through its SLIP transport.
            byte[] greeting = readPacket(COMMAND_TIMEOUT_MS);
            if (greeting[0] != 'O' || greeting[1] != 'H'
                    || greeting[2] != 'A' || greeting[3] != 'I') {
                throw new IOException("ESP32 stub 启动响应无效");
            }
            stubRunning = true;
            log(listener, "ESP32-S3 Web flasher stub 已启动");
        } catch (JSONException | IllegalArgumentException error) {
            throw new IOException("ESP32-S3 Web 烧录 stub 无效", error);
        }
    }

    private void uploadStubPart(byte[] data,
                                int address,
                                ProgressListener listener) throws IOException {
        if (data.length == 0) return;
        int blocks = (data.length + RAM_BLOCK_SIZE - 1) / RAM_BLOCK_SIZE;
        ByteBuffer begin = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        begin.putInt(data.length).putInt(blocks).putInt(RAM_BLOCK_SIZE).putInt(address);
        command(ESP_MEM_BEGIN, begin.array(), COMMAND_TIMEOUT_MS);
        for (int sequence = 0, offset = 0; offset < data.length; sequence++) {
            int length = Math.min(RAM_BLOCK_SIZE, data.length - offset);
            ByteBuffer body = ByteBuffer.allocate(16 + length).order(ByteOrder.LITTLE_ENDIAN);
            body.putInt(length).putInt(sequence).putInt(0).putInt(0);
            body.put(data, offset, length);
            commandWithChecksum(ESP_MEM_DATA, body.array(), data, offset, length, COMMAND_TIMEOUT_MS);
            offset += length;
        }
        log(listener, String.format("上传 flasher stub address=0x%08X bytes=%d", address, data.length));
    }

    private static byte[] padToFour(byte[] data) {
        int paddedLength = (data.length + 3) & ~3;
        if (paddedLength == data.length) return data;
        byte[] padded = new byte[paddedLength];
        System.arraycopy(data, 0, padded, 0, data.length);
        return padded;
    }

    private static byte[] deflate(byte[] data) throws IOException {
        Deflater compressor = new Deflater(9);
        compressor.setInput(data);
        compressor.finish();
        ByteArrayOutputStream output = new ByteArrayOutputStream(data.length);
        byte[] buffer = new byte[BLOCK_SIZE];
        try {
            while (!compressor.finished()) {
                int count = compressor.deflate(buffer);
                if (count == 0 && !compressor.finished()) {
                    throw new IOException("固件压缩失败");
                }
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        } finally {
            compressor.end();
        }
    }

    private void sync(ProgressListener listener) throws IOException {
        byte[] payload = new byte[36];
        payload[0] = 0x07;
        payload[1] = 0x07;
        payload[2] = 0x12;
        payload[3] = 0x20;
        for (int i = 4; i < payload.length; i++) payload[i] = 0x55;
        IOException last = null;
        for (int attempt = 0; attempt < 8; attempt++) {
            log(listener, "同步尝试 " + (attempt + 1) + "/8");
            try {
                command(ESP_SYNC, payload, COMMAND_TIMEOUT_MS);
                log(listener, "ESP32 ROM Bootloader 已响应");
                // ROM loaders commonly emit additional sync responses. The
                // reference esptool consumes seven more frames before sending
                // FLASH_BEGIN; leaving them queued shifts every next response.
                for (int extra = 0; extra < 7; extra++) {
                    try {
                        readPacket(250);
                    } catch (IOException ignored) {
                        break;
                    }
                }
                return;
            } catch (IOException error) {
                last = error;
                log(listener, "同步失败：" + error.getMessage());
                sleep(120);
            }
        }
        throw new IOException("无法进入 ESP32 下载模式，请按住 BOOT 后重新插入 USB，再重试", last);
    }

    private byte[] command(int opcode, byte[] payload, int timeoutMs) throws IOException {
        return commandWithChecksum(opcode, payload, null, 0, 0, timeoutMs);
    }

    private byte[] commandWithChecksum(int opcode,
                                        byte[] payload,
                                        byte[] checksumSource,
                                        int checksumOffset,
                                        int checksumLength,
                                        int timeoutMs) throws IOException {
        int checksum = checksumSource == null
                ? 0
                : checksum(checksumSource, checksumOffset, checksumLength);
        byte[] packet = slipEncode(buildPacket(opcode, payload, checksum));
        int sent = connection.bulkTransfer(output, packet, packet.length, USB_TIMEOUT_MS);
        if (sent != packet.length) throw new IOException("USB 写入失败（" + sent + "/" + packet.length + "）");
        long deadline = System.currentTimeMillis() + timeoutMs;
        int mismatchedOpcode = -1;
        while (System.currentTimeMillis() < deadline) {
            int remaining = (int) Math.max(1, deadline - System.currentTimeMillis());
            byte[] response;
            try {
                response = readPacket(remaining);
            } catch (IOException error) {
                if (mismatchedOpcode >= 0) {
                    throw new IOException("ESP32 响应命令不匹配 expected=0x"
                            + Integer.toHexString(opcode) + " got=0x"
                            + Integer.toHexString(mismatchedOpcode), error);
                }
                throw error;
            }
            if (response.length < 8 || (response[0] & 0xFF) != 0x01) continue;
            int responseOpcode = response[1] & 0xFF;
            if (responseOpcode != opcode) {
                mismatchedOpcode = responseOpcode;
                continue;
            }
            int status = response.length >= 10 ? (response[8] & 0xFF) | ((response[9] & 0xFF) << 8) : 0;
            if (status != 0) throw new IOException("ESP32 Bootloader 返回错误码 0x" + Integer.toHexString(status));
            return response;
        }
        throw new IOException("等待 ESP32 命令响应超时 expected=0x" + Integer.toHexString(opcode));
    }

    private byte[] readPacket(int timeoutMs) throws IOException {
        ByteArrayOutputStream encoded = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        long deadline = System.currentTimeMillis() + timeoutMs;
        boolean started = false;
        while (System.currentTimeMillis() < deadline) {
            int remaining = (int) Math.max(1, Math.min(USB_TIMEOUT_MS, deadline - System.currentTimeMillis()));
            int count = readUsb(buffer, buffer.length, remaining);
            if (count <= 0) continue;
            for (int i = 0; i < count; i++) {
                int value = buffer[i] & 0xFF;
                if (value == 0xC0) {
                    if (started && encoded.size() > 0) {
                        preserveInput(buffer, i + 1, count);
                        return slipDecode(encoded.toByteArray());
                    }
                    started = true;
                    encoded.reset();
                } else if (started) {
                    encoded.write(value);
                }
            }
        }
        throw new IOException("等待 ESP32 响应超时");
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

    private void preserveInput(byte[] buffer, int offset, int count) {
        if (offset >= count) return;
        pendingInput = Arrays.copyOfRange(buffer, offset, count);
        pendingInputOffset = 0;
    }

    private static byte[] readAll(InputStream input) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
        return output.toByteArray();
    }

    private byte[] buildPacket(int opcode, byte[] payload, int checksum) {
        ByteBuffer packet = ByteBuffer.allocate(8 + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        packet.put((byte) 0x00).put((byte) opcode).putShort((short) payload.length).putInt(checksum);
        packet.put(payload);
        return packet.array();
    }

    private static int checksum(byte[] data) {
        return checksum(data, 0, data.length);
    }

    private static int checksum(byte[] data, int offset, int length) {
        int value = ESP_CHECKSUM_MAGIC;
        for (int index = offset; index < offset + length; index++) value ^= data[index] & 0xFF;
        return value;
    }

    private static byte[] slipEncode(byte[] data) {
        ByteArrayOutputStream output = new ByteArrayOutputStream(data.length + 8);
        output.write(0xC0);
        for (byte item : data) {
            int value = item & 0xFF;
            if (value == 0xC0) output.write(new byte[]{(byte) 0xDB, (byte) 0xDC}, 0, 2);
            else if (value == 0xDB) output.write(new byte[]{(byte) 0xDB, (byte) 0xDD}, 0, 2);
            else output.write(value);
        }
        output.write(0xC0);
        return output.toByteArray();
    }

    private static byte[] slipDecode(byte[] data) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream(data.length);
        for (int i = 0; i < data.length; i++) {
            int value = data[i] & 0xFF;
            if (value == 0xDB) {
                if (++i >= data.length) throw new IOException("ESP32 SLIP 响应不完整");
                int escaped = data[i] & 0xFF;
                if (escaped == 0xDC) output.write(0xC0);
                else if (escaped == 0xDD) output.write(0xDB);
                else throw new IOException("ESP32 SLIP 响应转义错误");
            } else {
                output.write(value);
            }
        }
        return output.toByteArray();
    }

    private void resetToBootloader(ProgressListener listener) throws IOException {
        // This is esptool's USBJTAGSerialReset sequence. It differs from the
        // classic UART adapter sequence and keeps IO0 asserted while EN is
        // pulsed, so the S3 enters ROM download mode without pressing BOOT.
        setLineState(false, false, listener);
        sleep(100);
        setLineState(true, false, listener);
        sleep(100);
        setLineState(true, true, listener);
        setLineState(false, true, listener);
        sleep(100);
        setLineState(false, false, listener);
        sleep(120);
    }

    private void resetToApplication(ProgressListener listener) throws IOException {
        setLineState(false, true, listener);
        sleep(200);
        setLineState(false, false, listener);
        sleep(200);
        sleep(1200);
    }

    private void log(ProgressListener listener, String message) {
        if (listener != null) listener.onLog(message);
    }

    private String describeUsb() {
        return "VID=0x" + Integer.toHexString(vendorId)
                + " PID=0x" + Integer.toHexString(productId)
                + " interface=" + dataInterface.getId()
                + " IN=0x" + Integer.toHexString(input.getAddress())
                + " OUT=0x" + Integer.toHexString(output.getAddress());
    }

    private void setLineState(boolean dtr, boolean rts, ProgressListener listener) {
        int value = (dtr ? 1 : 0) | (rts ? 2 : 0);
        int result = connection.controlTransfer(0x21, 0x22, value, controlInterfaceId, null, 0, 100);
        log(listener, "SET_CONTROL_LINE_STATE dtr=" + dtr + " rts=" + rts + " result=" + result);
    }

    private static int align(int value, int alignment) {
        return ((value + alignment - 1) / alignment) * alignment;
    }

    private static byte[] littleEndianInt(int value) {
        return ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array();
    }

    private static void sleep(long milliseconds) throws IOException {
        try {
            Thread.sleep(milliseconds);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IOException("固件烧录被中断", interrupted);
        }
    }

    @Override
    public void close() {
        connection.releaseInterface(dataInterface);
        connection.close();
    }
}
