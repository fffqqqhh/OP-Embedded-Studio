import type { EmbeddedDisplayProfile, EmbeddedImagePayload } from '../model/types'
import {
  encodeUsbSequenceFrames,
  imageFilesToUsbSequence,
  sequenceContentCapacityBytes,
  type SequenceEncodingOptions,
  type UsbImageSequencePayload
} from './usb-sequence'

const WIRELESS_SEQUENCE_ENCODING = { allowPatches: false } as const

type WirelessSequenceOptions = Omit<SequenceEncodingOptions, 'allowPatches'>

export type WirelessImageSequencePayload = UsbImageSequencePayload

export function isWirelessSingleImagePayload(
  payload: EmbeddedImagePayload | null,
  profileId: string
): payload is EmbeddedImagePayload {
  return payload?.profileId === profileId && payload.frameCount === 1
}

function ensureWirelessSequenceFits(
  payload: UsbImageSequencePayload,
  maxContentBytes: number
): WirelessImageSequencePayload {
  if (payload.storedBytes > maxContentBytes) {
    throw new Error(
      `图片序列压缩后为 ${(payload.storedBytes / 1024 / 1024).toFixed(2)} MiB，超过 ${(maxContentBytes / 1024 / 1024).toFixed(2)} MiB 无线传输上限`
    )
  }
  return payload
}

export function encodeWifiSequenceFrames(
  profile: EmbeddedDisplayProfile,
  frames: Uint8Array[],
  name = 'Image sequence',
  options: WirelessSequenceOptions = {}
): WirelessImageSequencePayload {
  return ensureWirelessSequenceFits(
    encodeUsbSequenceFrames(profile, frames, name, {
      ...options,
      ...WIRELESS_SEQUENCE_ENCODING
    }),
    sequenceContentCapacityBytes(profile)
  )
}

export function encodeBleSequenceFrames(
  profile: EmbeddedDisplayProfile,
  frames: Uint8Array[],
  name = 'Image sequence',
  options: WirelessSequenceOptions = {}
): WirelessImageSequencePayload {
  return ensureWirelessSequenceFits(
    encodeUsbSequenceFrames(profile, frames, name, {
      ...options,
      ...WIRELESS_SEQUENCE_ENCODING
    }),
    sequenceContentCapacityBytes(profile)
  )
}

export async function imageFilesToWifiSequence(
  files: File[],
  profile: EmbeddedDisplayProfile,
  options: WirelessSequenceOptions = {}
): Promise<WirelessImageSequencePayload> {
  return ensureWirelessSequenceFits(
    await imageFilesToUsbSequence(files, profile, {
      ...options,
      ...WIRELESS_SEQUENCE_ENCODING
    }),
    sequenceContentCapacityBytes(profile)
  )
}

export async function imageFilesToBleSequence(
  files: File[],
  profile: EmbeddedDisplayProfile,
  options: WirelessSequenceOptions = {}
): Promise<WirelessImageSequencePayload> {
  return ensureWirelessSequenceFits(
    await imageFilesToUsbSequence(files, profile, {
      ...options,
      ...WIRELESS_SEQUENCE_ENCODING
    }),
    sequenceContentCapacityBytes(profile)
  )
}
