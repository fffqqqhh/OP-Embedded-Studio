import type { EmbeddedDisplayProfile } from '../model/types'
import { imageDataToRgb565, type EmbeddedImagePlacement } from './image'

export interface VideoRgb565Frames {
  frames: Uint8Array[]
  frameRate: number
  durationMs: number
}

function waitForEvent(target: EventTarget, eventName: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const handleEvent = () => {
      target.removeEventListener(eventName, handleEvent)
      target.removeEventListener('error', handleError)
      resolve()
    }
    const handleError = () => {
      target.removeEventListener(eventName, handleEvent)
      target.removeEventListener('error', handleError)
      reject(new Error('无法读取视频内容'))
    }
    target.addEventListener(eventName, handleEvent, { once: true })
    target.addEventListener('error', handleError, { once: true })
  })
}

function drawVideoFrame(
  video: HTMLVideoElement,
  context: CanvasRenderingContext2D,
  profile: EmbeddedDisplayProfile,
  placement: EmbeddedImagePlacement,
  backgroundColor: string
): Uint8Array {
  const width = profile.resolution.width
  const height = profile.resolution.height
  context.fillStyle = backgroundColor
  context.fillRect(0, 0, width, height)
  if (placement === 'stretch') {
    context.drawImage(video, 0, 0, width, height)
  } else {
    const scale = placement === 'pixel-perfect'
      ? 1
      : Math.min(width / video.videoWidth, height / video.videoHeight)
    const drawWidth = placement === 'pixel-perfect'
      ? Math.min(video.videoWidth, width)
      : Math.round(video.videoWidth * scale)
    const drawHeight = placement === 'pixel-perfect'
      ? Math.min(video.videoHeight, height)
      : Math.round(video.videoHeight * scale)
    const sourceX = placement === 'pixel-perfect' ? Math.max(0, Math.floor((video.videoWidth - width) / 2)) : 0
    const sourceY = placement === 'pixel-perfect' ? Math.max(0, Math.floor((video.videoHeight - height) / 2)) : 0
    context.imageSmoothingEnabled = placement !== 'pixel-perfect'
    context.drawImage(
      video,
      sourceX,
      sourceY,
      drawWidth,
      drawHeight,
      Math.round((width - drawWidth) / 2),
      Math.round((height - drawHeight) / 2),
      drawWidth,
      drawHeight
    )
  }
  return imageDataToRgb565(context.getImageData(0, 0, width, height).data, profile)
}

export async function videoFileToRgb565Frames(
  file: File,
  profile: EmbeddedDisplayProfile,
  options: {
    frameRate?: number
    placement?: EmbeddedImagePlacement
    backgroundColor?: string
  } = {}
): Promise<VideoRgb565Frames> {
  const frameRate = Math.max(1, options.frameRate ?? 20)
  const video = document.createElement('video')
  const objectUrl = URL.createObjectURL(file)
  video.preload = 'auto'
  video.muted = true
  video.playsInline = true
  video.src = objectUrl
  try {
    await waitForEvent(video, 'loadedmetadata')
    if (!Number.isFinite(video.duration) || video.duration <= 0) {
      throw new Error('视频没有可用的播放时长')
    }
    const frameCount = Math.max(2, Math.ceil(video.duration * frameRate))
    if (frameCount > 0xffff) throw new Error('视频帧数超过设备格式限制，请降低帧率')
    const canvas = document.createElement('canvas')
    canvas.width = profile.resolution.width
    canvas.height = profile.resolution.height
    const context = canvas.getContext('2d')
    if (!context) throw new Error('无法创建视频预览画布')
    const frames: Uint8Array[] = []
    const durationSeconds = video.duration
    for (let index = 0; index < frameCount; index += 1) {
      const timestamp = Math.min(
        durationSeconds,
        index / frameRate
      )
      video.currentTime = timestamp
      await waitForEvent(video, 'seeked')
      frames.push(
        drawVideoFrame(
          video,
          context,
          profile,
          options.placement ?? 'contain',
          options.backgroundColor ?? '#000000'
        )
      )
    }
    return { frames, frameRate, durationMs: Math.round(video.duration * 1000) }
  } finally {
    video.removeAttribute('src')
    video.load()
    URL.revokeObjectURL(objectUrl)
  }
}
