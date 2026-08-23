import type { EmbeddedDisplayProfile } from '../model/types'
import { IS_BROWSER } from '@/constants'

const STORAGE_KEY = 'op-embedded:custom-screen-profiles:v1'
const PROFILE_SCHEMA_VERSION = 1

export interface CustomProfileExport {
  schemaVersion: number
  exportedAt: string
  profiles: EmbeddedDisplayProfile[]
}

function storage(): Storage | null {
  if (!IS_BROWSER) return null
  try { return window.localStorage } catch { return null }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null
}

function normalizeProfile(value: unknown): EmbeddedDisplayProfile | null {
  if (!isRecord(value)) return null
  const resolution = isRecord(value.resolution) ? value.resolution : {}
  const width = Number(resolution.width)
  const height = Number(resolution.height)
  const id = typeof value.id === 'string' ? value.id.trim() : ''
  const name = typeof value.name === 'string' ? value.name.trim() : ''
  const controller = typeof value.controller === 'string' ? value.controller.trim() : ''
  if (!id || !name || !controller || !Number.isInteger(width) || width < 1 || width > 8192) return null
  if (!Number.isInteger(height) || height < 1 || height > 8192) return null
  const gpio = Array.isArray(value.gpio)
    ? value.gpio
        .filter(isRecord)
        .map((entry) => ({
          signal: typeof entry.signal === 'string' ? entry.signal.trim() : '',
          gpio: typeof entry.gpio === 'string' ? entry.gpio.trim() : '',
          pin: typeof entry.pin === 'string' ? entry.pin.trim() : undefined,
          note: typeof entry.note === 'string' ? entry.note.trim() : undefined
        }))
        .filter((entry) => entry.signal && entry.gpio)
    : []
  const image = isRecord(value.image) ? value.image : undefined
  const colorOrder = image && typeof image.colorOrder === 'string' ? image.colorOrder : 'RGB'
  const byteOrder = image && typeof image.byteOrder === 'string' ? image.byteOrder : 'little'
  const transport = image && typeof image.transport === 'string' ? image.transport : 'SPI'
  return {
    id,
    name,
    controller,
    resolution: { width, height },
    interface: typeof value.interface === 'string' ? value.interface : '4-wire SPI',
    backgroundColor: typeof value.backgroundColor === 'string' ? value.backgroundColor : '#000000',
    description: typeof value.description === 'string' ? value.description : '用户自定义屏幕方案',
    verified: false,
    module: typeof value.module === 'string' ? value.module : undefined,
    driverIc: typeof value.driverIc === 'string' ? value.driverIc : controller,
    flashSize: typeof value.flashSize === 'string' ? value.flashSize : undefined,
    wirelessContentBytes:
      typeof value.wirelessContentBytes === 'number' ? value.wirelessContentBytes : undefined,
    visibleArea: {
      shape: isRecord(value.visibleArea) && typeof value.visibleArea.shape === 'string'
        ? value.visibleArea.shape
        : 'rectangle',
      descriptionZh: '用户自定义可视区域'
    },
    image: {
      pixelFormat: 'RGB565',
      colorOrder,
      byteOrder,
      rotation: image && typeof image.rotation === 'number' ? image.rotation : 0,
      xGap: image && typeof image.xGap === 'number' ? image.xGap : 0,
      yGap: image && typeof image.yGap === 'number' ? image.yGap : 0,
      transport
    },
    gpio,
    source: 'custom',
    firmwareAvailable: false
  }
}

export function loadCustomDisplayProfiles(): EmbeddedDisplayProfile[] {
  let raw: string | null = null
  try { raw = storage()?.getItem(STORAGE_KEY) ?? null } catch { return [] }
  if (!raw) return []
  try {
    const parsed: unknown = JSON.parse(raw)
    const values = isRecord(parsed) && Array.isArray(parsed.profiles) ? parsed.profiles : parsed
    return Array.isArray(values)
      ? values.map(normalizeProfile).filter((profile): profile is EmbeddedDisplayProfile => Boolean(profile))
      : []
  } catch {
    return []
  }
}

function persistCustomDisplayProfiles(profiles: EmbeddedDisplayProfile[]): void {
  const target = storage()
  if (!target) throw new Error('当前环境不支持本地保存屏幕方案')
  try {
    target.setItem(
      STORAGE_KEY,
      JSON.stringify({ schemaVersion: PROFILE_SCHEMA_VERSION, exportedAt: new Date().toISOString(), profiles })
    )
  } catch {
    throw new Error('屏幕方案保存失败，请检查浏览器存储空间或权限')
  }
}

export function saveCustomDisplayProfile(profile: EmbeddedDisplayProfile): EmbeddedDisplayProfile {
  const normalized = normalizeProfile(profile)
  if (!normalized) throw new Error('屏幕方案信息不完整，请检查名称、驱动和分辨率')
  const profiles = loadCustomDisplayProfiles().filter((item) => item.id !== normalized.id)
  profiles.push(normalized)
  persistCustomDisplayProfiles(profiles)
  return normalized
}

export function removeCustomDisplayProfile(profileId: string): void {
  persistCustomDisplayProfiles(loadCustomDisplayProfiles().filter((profile) => profile.id !== profileId))
}

export function exportCustomDisplayProfiles(): CustomProfileExport {
  return {
    schemaVersion: PROFILE_SCHEMA_VERSION,
    exportedAt: new Date().toISOString(),
    profiles: loadCustomDisplayProfiles()
  }
}

export function importCustomDisplayProfiles(value: unknown): EmbeddedDisplayProfile[] {
  const values = isRecord(value) && Array.isArray(value.profiles) ? value.profiles : value
  if (!Array.isArray(values)) throw new Error('配置文件格式不正确')
  const imported = values.map(normalizeProfile).filter((profile): profile is EmbeddedDisplayProfile => Boolean(profile))
  if (!imported.length) throw new Error('配置文件中没有有效的屏幕方案')
  const merged = [...loadCustomDisplayProfiles()]
  for (const profile of imported) {
    const index = merged.findIndex((item) => item.id === profile.id)
    if (index !== -1) merged[index] = profile
    else merged.push(profile)
  }
  persistCustomDisplayProfiles(merged)
  return imported
}
