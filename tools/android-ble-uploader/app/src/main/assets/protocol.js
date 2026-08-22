(function () {
  const PROFILES = Object.freeze({
    co5300_waveshare_amoled_1_75c: Object.freeze({
      id: 'co5300_waveshare_amoled_1_75c',
      name: 'Waveshare 1.75C · 466 × 466',
      width: 466,
      height: 466,
      shape: 'round',
      colorOrder: 'RGB',
      byteOrder: 'little',
      wirelessContentBytes: 0x1cf0000
    }),
    co5300_m5stack_stopwatch: Object.freeze({
      id: 'co5300_m5stack_stopwatch',
      name: 'M5Stack StopWatch · 466 × 466',
      width: 466,
      height: 466,
      shape: 'round',
      colorOrder: 'RGB',
      byteOrder: 'little',
      wirelessContentBytes: 0xcf0000
    }),
    ili9342_m5stack_cores3: Object.freeze({
      id: 'ili9342_m5stack_cores3',
      name: 'M5Stack CoreS3 · 320 × 240',
      width: 320,
      height: 240,
      shape: 'rectangle',
      colorOrder: 'BGR',
      byteOrder: 'little',
      wirelessContentBytes: 0xcf0000
    })
  })
  let activeProfile = PROFILES.co5300_waveshare_amoled_1_75c
  const CONTENT_MAGIC = 0x4f504331
  const CONTENT_VERSION = 1
  const HEADER_BYTES = 24
  const SEQUENCE_HEADER_BYTES = 12
  const RESOURCE_BYTES = 12

  function crc32(bytes) {
    let crc = 0xffffffff
    for (const byte of bytes) {
      crc ^= byte
      for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0)
    }
    return (crc ^ 0xffffffff) >>> 0
  }

  function envelope(mode, frameCount, payload) {
    const content = new Uint8Array(HEADER_BYTES + payload.byteLength)
    const view = new DataView(content.buffer)
    view.setUint32(0, CONTENT_MAGIC, true)
    view.setUint16(4, CONTENT_VERSION, true)
    view.setUint8(6, mode)
    view.setUint16(8, activeProfile.width, true)
    view.setUint16(10, activeProfile.height, true)
    view.setUint16(12, frameCount, true)
    view.setUint32(16, payload.byteLength, true)
    view.setUint32(20, crc32(payload), true)
    content.set(payload, HEADER_BYTES)
    if (content.byteLength > activeProfile.wirelessContentBytes) {
      throw new Error(`内容超过 ${formatMiB(activeProfile.wirelessContentBytes)} MiB 分区上限`)
    }
    return content
  }

  function rgb565(imageData) {
    const source = imageData.data
    const output = new Uint8Array(activeProfile.width * activeProfile.height * 2)
    for (let pixel = 0; pixel < activeProfile.width * activeProfile.height; pixel += 1) {
      const sourceOffset = pixel * 4
      const first = activeProfile.colorOrder === 'BGR' ? source[sourceOffset + 2] : source[sourceOffset]
      const last = activeProfile.colorOrder === 'BGR' ? source[sourceOffset] : source[sourceOffset + 2]
      const value =
        ((first & 0xf8) << 8) |
        ((source[sourceOffset + 1] & 0xfc) << 3) |
        (last >> 3)
      if (activeProfile.byteOrder === 'big') {
        output[pixel * 2] = value >> 8
        output[pixel * 2 + 1] = value & 0xff
      } else {
        output[pixel * 2] = value & 0xff
        output[pixel * 2 + 1] = value >> 8
      }
    }
    return output
  }

  function encodeFrame(frame) {
    const expectedBytes = activeProfile.width * activeProfile.height * 2
    if (frame.byteLength !== expectedBytes) {
      throw new Error(`帧尺寸不匹配：需要 ${activeProfile.width} × ${activeProfile.height} RGB565 数据`)
    }
    return envelope(0, 1, frame)
  }

  function encodeRle(frame) {
    const buffer = new Uint8Array(frame.byteLength * 2)
    let outputOffset = 0
    let offset = 0
    while (offset < frame.byteLength) {
      const low = frame[offset]
      const high = frame[offset + 1]
      let run = 1
      while (
        run < 0xffff &&
        offset + run * 2 < frame.byteLength &&
        frame[offset + run * 2] === low &&
        frame[offset + run * 2 + 1] === high
      ) run += 1
      buffer[outputOffset] = run & 0xff
      buffer[outputOffset + 1] = run >> 8
      buffer[outputOffset + 2] = low
      buffer[outputOffset + 3] = high
      outputOffset += 4
      offset += run * 2
    }
    return outputOffset < frame.byteLength
      ? { codec: 1, bytes: buffer.slice(0, outputOffset) }
      : { codec: 0, bytes: frame }
  }

  function sequenceContentBytes(encoded) {
    return HEADER_BYTES + SEQUENCE_HEADER_BYTES + encoded.length * RESOURCE_BYTES +
      encoded.reduce((total, frame) => total + frame.bytes.byteLength, 0)
  }

  function buildSequence(encoded, frameDelayMs) {
    const dataBytes = encoded.reduce((total, frame) => total + frame.bytes.byteLength, 0)
    const payload = new Uint8Array(SEQUENCE_HEADER_BYTES + encoded.length * RESOURCE_BYTES + dataBytes)
    const view = new DataView(payload.buffer)
    view.setUint32(0, activeProfile.width * activeProfile.height * 2, true)
    view.setUint16(4, Math.min(0xffff, Math.max(1, Math.round(frameDelayMs))), true)
    view.setUint16(6, encoded.length, true)
    view.setUint32(8, dataBytes, true)
    const dataOffset = SEQUENCE_HEADER_BYTES + encoded.length * RESOURCE_BYTES
    let storedOffset = 0
    encoded.forEach((frame, index) => {
      const resourceOffset = SEQUENCE_HEADER_BYTES + index * RESOURCE_BYTES
      view.setUint32(resourceOffset, storedOffset, true)
      view.setUint32(resourceOffset + 4, frame.bytes.byteLength, true)
      view.setUint8(resourceOffset + 8, frame.codec)
      payload.set(frame.bytes, dataOffset + storedOffset)
      storedOffset += frame.bytes.byteLength
    })
    return envelope(2, encoded.length, payload)
  }

  function encodeSequence(frames, frameDelayMs = 50) {
    if (frames.length < 2) throw new Error('PNG 序列至少需要两张图片')
    return buildSequence(frames.map(encodeRle), frameDelayMs)
  }

  function selectedFrameIndexes(totalFrames, targetFrames, strategy) {
    if (strategy === 'trim') return Array.from({ length: targetFrames }, (_, index) => index)
    return Array.from(
      { length: targetFrames },
      (_, index) => Math.floor(index * (totalFrames - 1) / (targetFrames - 1))
    )
  }

  function encodeSequenceToFit(frames, frameDelayMs = 50, strategy = 'speed') {
    if (frames.length < 2) throw new Error('PNG 序列至少需要两张图片')
    const encodedFrames = frames.map(encodeRle)
    for (let targetFrames = encodedFrames.length; targetFrames >= 2; targetFrames -= 1) {
      const indexes = selectedFrameIndexes(encodedFrames.length, targetFrames, strategy)
      const selected = indexes.map((index) => encodedFrames[index])
      if (sequenceContentBytes(selected) <= activeProfile.wirelessContentBytes) {
        return {
          content: buildSequence(selected, frameDelayMs),
          frameCount: selected.length,
          sourceFrameCount: encodedFrames.length,
          strategy,
          reduced: selected.length !== encodedFrames.length
        }
      }
    }
    throw new Error(`至少两帧内容仍超过 ${formatMiB(activeProfile.wirelessContentBytes)} MiB 分区上限，请缩短视频或降低帧率`)
  }

  function formatMiB(bytes) {
    return (bytes / 1024 / 1024).toFixed(2)
  }

  function setProfile(profileId) {
    const profile = PROFILES[profileId]
    if (!profile) throw new Error(`不支持的屏幕方案：${profileId}`)
    activeProfile = profile
    return profile
  }

  window.OpenPencilProtocol = {
    get WIDTH() { return activeProfile.width },
    get HEIGHT() { return activeProfile.height },
    get MAX_CONTENT_BYTES() { return activeProfile.wirelessContentBytes },
    PROFILES,
    getProfile: () => activeProfile,
    setProfile,
    rgb565,
    encodeFrame,
    encodeSequence,
    encodeSequenceToFit
  }
})()
