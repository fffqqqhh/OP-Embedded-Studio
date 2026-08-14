(function () {
  const protocol = window.OpenPencilProtocol
  const canvas = document.getElementById('previewCanvas')
  const context = canvas.getContext('2d', { willReadFrequently: true })
  const fileInput = document.getElementById('fileInput')
  const cameraButton = document.getElementById('cameraButton')
  const fpsInput = document.getElementById('fpsInput')
  const editButton = document.getElementById('editButton')
  const uploadButton = document.getElementById('uploadButton')
  const backgroundInput = document.getElementById('backgroundInput')
  const resetButton = document.getElementById('resetButton')
  const statusText = document.getElementById('statusText')
  const progressBar = document.getElementById('progressBar')
  const connectionBadge = document.getElementById('connectionBadge')
  const fileSummary = document.getElementById('fileSummary')
  const payloadSummary = document.getElementById('payloadSummary')
  const diagnosticText = document.getElementById('diagnosticText')
  const emptyPreview = document.getElementById('emptyPreview')
  const editHint = document.getElementById('editHint')
  const editControls = document.getElementById('editControls')
  const previewWrap = document.getElementById('previewWrap')

  const pointers = new Map()
  let files = []
  let previewBitmap = null
  let connected = false
  let connecting = false
  let busy = false
  let pendingUpload = false
  let editing = false
  let renderedProgressPercent = -1
  let lastProgressStatusAt = 0
  let lastDiagnosticAt = 0
  let zoom = 1
  let offsetX = 0
  let offsetY = 0
  let gesture = null
  const MAX_VIDEO_SECONDS = 4
  const MAX_VIDEO_FRAMES = 64

  function isVideoFile(file) {
    return Boolean(file?.type?.startsWith('video/'))
  }

  function selectedFps() {
    return Number(fpsInput.value) || 12
  }

  function setStatus(message, type = '') {
    statusText.textContent = message
    statusText.className = `status ${type}`
  }

  function setDiagnostic(message, force = false) {
    const now = performance.now()
    if (!force && now - lastDiagnosticAt < 1000) return
    lastDiagnosticAt = now
    diagnosticText.textContent = `BLE diagnostics: ${message}`
  }

  function updateConnectionBadge() {
    connectionBadge.className = 'badge'
    if (connected) {
      connectionBadge.textContent = '已连接'
      connectionBadge.classList.add('connected')
    } else if (connecting) {
      connectionBadge.textContent = '连接中'
      connectionBadge.classList.add('connecting')
    } else {
      connectionBadge.textContent = '按需连接'
    }
  }

  function updateActions() {
    fileInput.disabled = busy
    cameraButton.disabled = busy
    fpsInput.disabled = busy
    editButton.disabled = files.length === 0 || busy
    uploadButton.disabled = files.length === 0 || busy
    editButton.textContent = editing ? '完成' : '编辑'
    editButton.classList.toggle('active', editing)
  }

  function clearGesture() {
    pointers.clear()
    gesture = null
  }

  function setEditing(nextEditing) {
    editing = Boolean(nextEditing && previewBitmap && !busy)
    previewWrap.classList.toggle('editing', editing)
    previewWrap.classList.toggle('locked', !editing)
    editControls.classList.toggle('visible', editing)
    editControls.setAttribute('aria-hidden', String(!editing))
    editHint.hidden = !editing
    clearGesture()
    updateActions()
  }

  function resetCrop() {
    zoom = 1
    offsetX = 0
    offsetY = 0
    renderPreview()
  }

  function releaseBitmap(bitmap) {
    if (bitmap?.close) bitmap.close()
  }

  async function decodeWithImageElement(file) {
    const objectUrl = URL.createObjectURL(file)
    const image = new Image()
    image.decoding = 'async'
    try {
      await new Promise((resolve, reject) => {
        image.onload = resolve
        image.onerror = () => reject(new Error('HTML image decode failed'))
        image.src = objectUrl
      })
      if (!image.naturalWidth || !image.naturalHeight) throw new Error('Invalid image dimensions')
      return {
        source: image,
        width: image.naturalWidth,
        height: image.naturalHeight,
        close() {
          image.src = ''
          URL.revokeObjectURL(objectUrl)
        }
      }
    } catch (error) {
      image.src = ''
      URL.revokeObjectURL(objectUrl)
      throw error
    }
  }

  async function decodeImageFile(file) {
    if (!file || file.size <= 0) throw new Error('图片文件为空，请确认照片已完整下载到手机')
    if (typeof createImageBitmap === 'function') {
      try {
        const bitmap = await createImageBitmap(file)
        if (!bitmap.width || !bitmap.height) throw new Error('Invalid bitmap dimensions')
        return {
          source: bitmap,
          width: bitmap.width,
          height: bitmap.height,
          close() {
            bitmap.close()
          }
        }
      } catch {
        // Some Android gallery providers are not supported by createImageBitmap.
      }
    }
    try {
      return await decodeWithImageElement(file)
    } catch {
      const fileType = file.type || '未知格式'
      const fileSize = `${(file.size / 1024 / 1024).toFixed(2)} MiB`
      throw new Error(`无法读取图片（${fileType}，${fileSize}），请确认照片已完整下载到手机`)
    }
  }

  async function decodeVideoFile(file) {
    if (!file || file.size <= 0) throw new Error('视频文件为空，请确认视频已完整下载到手机')
    const objectUrl = URL.createObjectURL(file)
    const video = document.createElement('video')
    video.muted = true
    video.playsInline = true
    video.preload = 'auto'
    try {
      video.src = objectUrl
      await waitForVideoEvent(video, 'loadedmetadata', '无法读取视频，请选择本地视频文件')
      if (!video.videoWidth || !video.videoHeight || !Number.isFinite(video.duration)) {
        throw new Error('视频没有可用画面')
      }
      // WebView may expose metadata before its compositor has decoded a frame.
      await waitForVideoFrame(video, Math.min(0.001, Math.max(0, video.duration / 2)))
      return {
        source: video,
        width: video.videoWidth,
        height: video.videoHeight,
        duration: video.duration,
        close() {
          video.pause()
          video.removeAttribute('src')
          video.load()
          URL.revokeObjectURL(objectUrl)
        }
      }
    } catch (error) {
      video.removeAttribute('src')
      URL.revokeObjectURL(objectUrl)
      throw error
    }
  }

  function waitForVideoEvent(video, eventName, errorMessage, timeoutMs = 8000) {
    return new Promise((resolve, reject) => {
      const timer = window.setTimeout(() => finish(new Error(errorMessage)), timeoutMs)
      const onEvent = () => finish()
      const onError = () => finish(new Error(errorMessage))
      const finish = (error) => {
        window.clearTimeout(timer)
        video.removeEventListener(eventName, onEvent)
        video.removeEventListener('error', onError)
        if (error) reject(error)
        else resolve()
      }
      video.addEventListener(eventName, onEvent, { once: true })
      video.addEventListener('error', onError, { once: true })
    })
  }

  function waitForPresentedFrame(video) {
    return new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)))
  }

  async function waitForVideoFrame(video, time) {
    const duration = Number.isFinite(video.duration) ? video.duration : 0
    const target = duration > 0 ? Math.min(Math.max(0.001, time), Math.max(0.001, duration - 0.001)) : 0
    if (Math.abs(video.currentTime - target) > 0.0005 || video.readyState < HTMLMediaElement.HAVE_CURRENT_DATA) {
      const seeked = waitForVideoEvent(video, 'seeked', '视频帧读取超时', 6000)
      video.currentTime = target
      await seeked
    }
    await waitForPresentedFrame(video)
  }

  function drawBitmap(targetContext, bitmap) {
    const width = protocol.WIDTH
    const height = protocol.HEIGHT
    targetContext.fillStyle = backgroundInput.value
    targetContext.fillRect(0, 0, width, height)
    const baseScale = Math.max(width / bitmap.width, height / bitmap.height)
    const scale = baseScale * zoom
    const drawWidth = bitmap.width * scale
    const drawHeight = bitmap.height * scale
    const drawX = (width - drawWidth) / 2 + offsetX
    const drawY = (height - drawHeight) / 2 + offsetY
    const visibleLeft = Math.max(0, drawX)
    const visibleTop = Math.max(0, drawY)
    const visibleRight = Math.min(width, drawX + drawWidth)
    const visibleBottom = Math.min(height, drawY + drawHeight)
    const visibleWidth = visibleRight - visibleLeft
    const visibleHeight = visibleBottom - visibleTop
    if (visibleWidth <= 0 || visibleHeight <= 0) return
    targetContext.imageSmoothingEnabled = true
    targetContext.imageSmoothingQuality = 'high'
    targetContext.drawImage(
      bitmap.source,
      (visibleLeft - drawX) / scale,
      (visibleTop - drawY) / scale,
      visibleWidth / scale,
      visibleHeight / scale,
      visibleLeft,
      visibleTop,
      visibleWidth,
      visibleHeight
    )
  }

  function renderPreview() {
    context.fillStyle = backgroundInput.value
    context.fillRect(0, 0, protocol.WIDTH, protocol.HEIGHT)
    if (previewBitmap) drawBitmap(context, previewBitmap)
  }

  async function loadPreview() {
    releaseBitmap(previewBitmap)
    previewBitmap = files.length
      ? isVideoFile(files[0])
        ? await decodeVideoFile(files[0])
        : await decodeImageFile(files[0])
      : null
    emptyPreview.hidden = Boolean(previewBitmap)
    setEditing(Boolean(previewBitmap))
    resetCrop()
  }

  async function renderFile(file) {
    const bitmap = await decodeImageFile(file)
    try {
      const frameCanvas = document.createElement('canvas')
      frameCanvas.width = protocol.WIDTH
      frameCanvas.height = protocol.HEIGHT
      const frameContext = frameCanvas.getContext('2d', { willReadFrequently: true })
      drawBitmap(frameContext, bitmap)
      return protocol.rgb565(frameContext.getImageData(0, 0, protocol.WIDTH, protocol.HEIGHT))
    } finally {
      releaseBitmap(bitmap)
    }
  }

  async function renderVideoFrames(file) {
    const video = await decodeVideoFile(file)
    try {
      const fps = selectedFps()
      const duration = Math.min(video.duration, MAX_VIDEO_SECONDS)
      const frameCount = Math.min(MAX_VIDEO_FRAMES, Math.max(2, Math.ceil(duration * fps)))
      const frameCanvas = document.createElement('canvas')
      frameCanvas.width = protocol.WIDTH
      frameCanvas.height = protocol.HEIGHT
      const frameContext = frameCanvas.getContext('2d', { willReadFrequently: true })
      const frames = []
      for (let index = 0; index < frameCount; index += 1) {
        const time = Math.min(duration - 0.001, index / fps)
        setStatus(`正在处理视频：${index + 1} / ${frameCount} 帧`)
        await waitForVideoFrame(video.source, Math.max(0, time))
        drawBitmap(frameContext, video)
        frames.push(protocol.rgb565(frameContext.getImageData(0, 0, protocol.WIDTH, protocol.HEIGHT)))
      }
      return { frames, frameCount, fps, duration }
    } finally {
      releaseBitmap(video)
    }
  }

  function bytesToBase64(bytes) {
    let binary = ''
    const chunk = 0x4000
    for (let offset = 0; offset < bytes.length; offset += chunk) {
      binary += String.fromCharCode(...bytes.subarray(offset, offset + chunk))
    }
    return btoa(binary)
  }

  function sendPayloadToNative(content) {
    let error = window.OpenPencilNative.beginPayload(content.byteLength)
    if (error) throw new Error(error)
    const chunkBytes = 48 * 1024
    for (let offset = 0; offset < content.byteLength; offset += chunkBytes) {
      error = window.OpenPencilNative.appendPayloadChunk(
        bytesToBase64(content.subarray(offset, Math.min(offset + chunkBytes, content.byteLength)))
      )
      if (error) throw new Error(error)
    }
    error = window.OpenPencilNative.finishPayload()
    if (error) throw new Error(error)
  }

  function startNativeUpload() {
    pendingUpload = false
    setStatus('设备已连接，正在上传…')
    window.OpenPencilNative.upload()
  }

  async function processAndUpload() {
    if (!window.OpenPencilNative) return setStatus('原生 BLE 桥不可用', 'error')
    setEditing(false)
    busy = true
    pendingUpload = false
    updateActions()
    progressBar.style.width = '0%'
    renderedProgressPercent = 0
    try {
      const containsVideo = files.some(isVideoFile)
      if (containsVideo && files.length !== 1) {
        throw new Error('视频需要单独上传，不能与照片或 PNG 序列混合')
      }
      if (!containsVideo && files.length > 1 && files.some((file) => !file.name.toLowerCase().endsWith('.png'))) {
        throw new Error('多帧序列只支持 PNG 文件')
      }
      let frames = []
      let videoDetails = null
      if (containsVideo) {
        videoDetails = await renderVideoFrames(files[0])
        frames = videoDetails.frames
      } else {
        const sorted = [...files].sort((left, right) =>
          left.name.localeCompare(right.name, undefined, { numeric: true, sensitivity: 'base' })
        )
        for (let index = 0; index < sorted.length; index += 1) {
          setStatus(`正在处理第 ${index + 1} / ${sorted.length} 帧…`)
          frames.push(await renderFile(sorted[index]))
        }
      }
      const content = frames.length === 1
        ? protocol.encodeFrame(frames[0])
        : protocol.encodeSequence(frames, 1000 / (videoDetails?.fps || 20))
      payloadSummary.textContent = videoDetails
        ? `${frames.length} 帧 · ${videoDetails.fps} FPS · ${videoDetails.duration.toFixed(1)} 秒 · ${(content.byteLength / 1024 / 1024).toFixed(2)} MiB`
        : `${frames.length} 帧 · ${(content.byteLength / 1024 / 1024).toFixed(2)} MiB`
      setStatus(
        videoDetails
          ? `视频已转换为 ${frames.length} 帧，正在查找设备…`
          : '内容已准备，正在查找设备…'
      )
      sendPayloadToNative(content)
      if (connected) {
        startNativeUpload()
      } else {
        pendingUpload = true
        connecting = true
        updateConnectionBadge()
        window.OpenPencilNative.connect()
      }
    } catch (error) {
      busy = false
      pendingUpload = false
      connecting = false
      updateConnectionBadge()
      updateActions()
      setStatus(error instanceof Error ? error.message : String(error), 'error')
    }
  }

  function canvasPoint(event) {
    const bounds = previewWrap.getBoundingClientRect()
    const scale = protocol.WIDTH / bounds.width
    return {
      x: (event.clientX - bounds.left) * scale - protocol.WIDTH / 2,
      y: (event.clientY - bounds.top) * scale - protocol.HEIGHT / 2
    }
  }

  function pointerPair() {
    return [...pointers.values()].slice(0, 2)
  }

  function beginGesture() {
    if (pointers.size === 1) {
      const point = [...pointers.values()][0]
      gesture = { type: 'pan', point, offsetX, offsetY }
      return
    }
    if (pointers.size >= 2) {
      const [first, second] = pointerPair()
      const center = { x: (first.x + second.x) / 2, y: (first.y + second.y) / 2 }
      gesture = {
        type: 'pinch',
        distance: Math.hypot(second.x - first.x, second.y - first.y),
        center,
        zoom,
        offsetX,
        offsetY
      }
    }
  }

  async function handleFileSelection(input) {
    files = [...input.files]
    if (files.some(isVideoFile) && files.length > 1) {
      files = [files.find(isVideoFile)]
      input.value = ''
      setStatus('视频将单独处理，已忽略同次选择的其他文件')
    }
    fileSummary.textContent = files.length
      ? isVideoFile(files[0])
        ? `短视频 · ${selectedFps()} FPS · 最多 ${MAX_VIDEO_SECONDS} 秒`
        : files.length === 1
          ? '单帧图片 · 466 × 466'
          : `${files.length} 帧 PNG 序列 · 20 FPS`
      : '支持照片、PNG 序列或短视频，目标 466 × 466。'
    payloadSummary.textContent = '尚未准备内容'
    progressBar.style.width = '0%'
    setStatus(files.length ? '编辑已激活，调整完成后点击“完成”' : '选择图片后即可上传')
    try {
      await loadPreview()
    } catch (error) {
      files = []
      input.value = ''
      releaseBitmap(previewBitmap)
      previewBitmap = null
      emptyPreview.hidden = false
      setEditing(false)
      fileSummary.textContent = '支持单图或 PNG 序列，目标 466 × 466。'
      setStatus(error instanceof Error ? error.message : String(error), 'error')
    } finally {
      updateActions()
    }
  }

  fileInput.addEventListener('change', () => handleFileSelection(fileInput))
  cameraButton.addEventListener('click', () => {
    if (window.OpenPencilNative?.capturePhoto) window.OpenPencilNative.capturePhoto()
    else setStatus('相机功能只在 Android 应用内可用', 'error')
  })
  document.querySelector('.action-icon[title="导入照片或视频"]')?.addEventListener('click', (event) => {
    if (window.OpenPencilNative?.pickMedia) {
      event.preventDefault()
      window.OpenPencilNative.pickMedia()
    }
  })
  fpsInput.value = localStorage.getItem('openpencil-video-fps') || '12'
  fpsInput.addEventListener('change', () => {
    localStorage.setItem('openpencil-video-fps', fpsInput.value)
    if (files.length && isVideoFile(files[0])) {
      fileSummary.textContent = `短视频 · ${selectedFps()} FPS · 最多 ${MAX_VIDEO_SECONDS} 秒`
    }
  })

  editButton.addEventListener('click', () => setEditing(!editing))
  uploadButton.addEventListener('click', processAndUpload)
  resetButton.addEventListener('click', resetCrop)
  backgroundInput.addEventListener('input', renderPreview)

  previewWrap.addEventListener('pointerdown', (event) => {
    if (!editing || !previewBitmap) return
    event.preventDefault()
    pointers.set(event.pointerId, canvasPoint(event))
    previewWrap.setPointerCapture(event.pointerId)
    beginGesture()
  })

  previewWrap.addEventListener('pointermove', (event) => {
    if (!editing || !pointers.has(event.pointerId)) return
    event.preventDefault()
    pointers.set(event.pointerId, canvasPoint(event))
    if (pointers.size === 1 && gesture?.type === 'pan') {
      const point = [...pointers.values()][0]
      offsetX = gesture.offsetX + point.x - gesture.point.x
      offsetY = gesture.offsetY + point.y - gesture.point.y
      renderPreview()
      return
    }
    if (pointers.size >= 2 && gesture?.type === 'pinch') {
      const [first, second] = pointerPair()
      const distance = Math.max(1, Math.hypot(second.x - first.x, second.y - first.y))
      const center = { x: (first.x + second.x) / 2, y: (first.y + second.y) / 2 }
      const nextZoom = Math.min(6, Math.max(0.2, gesture.zoom * distance / Math.max(1, gesture.distance)))
      const ratio = nextZoom / gesture.zoom
      zoom = nextZoom
      offsetX = center.x - (gesture.center.x - gesture.offsetX) * ratio
      offsetY = center.y - (gesture.center.y - gesture.offsetY) * ratio
      renderPreview()
    }
  })

  function releasePointer(event) {
    if (!pointers.has(event.pointerId)) return
    pointers.delete(event.pointerId)
    beginGesture()
  }

  previewWrap.addEventListener('pointerup', releasePointer)
  previewWrap.addEventListener('pointercancel', releasePointer)
  previewWrap.addEventListener('lostpointercapture', releasePointer)

  window.OpenPencilApp = {
    async nativeMedia(url, mimeType) {
      try {
        const response = await fetch(url)
        if (!response.ok) throw new Error(`媒体文件读取失败 (${response.status})`)
        const blob = await response.blob()
        if (!blob.size) throw new Error('媒体文件为空，请重新选择或拍摄')
        const file = new File([blob], mimeType.startsWith('video/') ? 'captured-video.mp4' : 'captured-image.jpg', { type: mimeType })
        files = [file]
        fileSummary.textContent = isVideoFile(file)
          ? `短视频 · ${selectedFps()} FPS · 最多 ${MAX_VIDEO_SECONDS} 秒`
          : '单帧图片 · 466 × 466'
        payloadSummary.textContent = '媒体已载入，点击上传到设备'
        await loadPreview()
        updateActions()
        setStatus('媒体已载入，可以编辑或上传')
      } catch (error) {
        setStatus(error instanceof Error ? error.message : String(error), 'error')
      }
    },
    nativeEvent(event) {
      if (event.type === 'connected') {
        connected = true
        connecting = false
        updateConnectionBadge()
        if (pendingUpload) startNativeUpload()
        else setStatus(event.message, 'success')
      } else if (event.type === 'disconnected') {
        connected = false
        connecting = false
        pendingUpload = false
        busy = false
        updateConnectionBadge()
        setStatus(event.message)
      } else if (event.type === 'progress') {
        const percent = event.total ? Math.round(event.written / event.total * 100) : 0
        if (percent !== renderedProgressPercent) {
          renderedProgressPercent = percent
          progressBar.style.width = `${percent}%`
        }
        const now = performance.now()
        if (percent === 100 || now - lastProgressStatusAt >= 500) {
          lastProgressStatusAt = now
          setStatus(`${event.message}：${percent}%`)
        }
      } else if (event.type === 'complete') {
        busy = false
        pendingUpload = false
        progressBar.style.width = '100%'
        setStatus(event.message, 'success')
      } else if (event.type === 'error') {
        busy = false
        pendingUpload = false
        if (connecting) connected = false
        connecting = false
        updateConnectionBadge()
        setStatus(event.message, 'error')
      } else if (event.type === 'diagnostic') {
        setDiagnostic(
          event.message,
          event.message.startsWith('READY') || event.message.startsWith('LINK: disconnected')
        )
      } else {
        setStatus(event.message)
      }
      updateActions()
    }
  }

  updateConnectionBadge()
  renderPreview()
  updateActions()
})()
