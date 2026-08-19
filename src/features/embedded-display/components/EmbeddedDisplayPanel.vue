<script setup lang="ts">
import { computed, onUnmounted, ref, watch } from 'vue'

import AppSelect from '@/components/ui/AppSelect.vue'
import { PanelHeader, PanelSection } from '@/components/ui/panel'
import SegmentedControl from '@/components/ui/SegmentedControl.vue'

import { prepareWifiFirmwareCredentials } from '../adapters/http'
import { embeddedImagePlacementLabel, type EmbeddedImagePlacement } from '../adapters/image'
import { flashFirmwareManifest } from '../adapters/manifest-firmware'
import {
  transferUsbContentWithFirmwareFallback,
  type UsbContentFirmwareStage
} from '../adapters/usb-content-firmware'
import { clearActiveUsbPort, withUsbDeploymentLock } from '../adapters/usb-deployment-lock'
import {
  probeWirelessDevice,
  uploadWirelessImage,
  uploadWirelessPrototype,
  uploadWirelessSequence
} from '../adapters/wireless'
import {
  imageFilesToBleSequence,
  imageFilesToWifiSequence,
  isWirelessSingleImagePayload,
  type WirelessImageSequencePayload
} from '../adapters/wireless-sequence'
import {
  flashUsbFrameFirmware,
  flashUsbAnimatedPrototypeFirmware,
  flashUsbPrototypeFirmware,
  flashUsbSequenceFirmware,
  supportsUsbFrameFastFlash,
  type UsbContentBuildMode,
  type UsbFlashOptions
} from '../adapters/usb-content'
import type { UsbContentSerialPort } from '../adapters/usb-content-transfer'
import { imageFilesToUsbSequence } from '../adapters/usb-sequence'
import { encodeUsbAnimatedPrototype } from '../adapters/animated-prototype'
import type { SerialPortLike } from '../adapters/serial-flasher'
import { useBleDeviceSession } from '../composables/useBleDeviceSession'
import { useEmbeddedDisplay } from '../composables/useEmbeddedDisplay'
import { useSerialDeviceSession } from '../composables/useSerialDeviceSession'
import DevicePrototypePreview from '@/features/device-prototype/components/DevicePrototypePreview.vue'
import EmbeddedDisplayContentPreview from './EmbeddedDisplayContentPreview.vue'
import WifiLiveMirrorPanel from '../live-mirror/components/WifiLiveMirrorPanel.vue'
import { embeddedDisplayAdvancedDebugMode } from '../debug'
import type {
  DevicePrototypeFrameRender,
  DevicePrototypeInteraction
} from '@/features/device-prototype/model/types'
import type {
  EmbeddedBuildMode,
  EmbeddedFrameBake,
  EmbeddedFrameBakeById,
  EmbeddedFrameBakeState,
  EmbeddedAnimatedPrototypeBake,
  EmbeddedAnimatedPrototypeBakeResult,
  EmbeddedImagePayload,
  EmbeddedPrototypeBake,
  EmbeddedPrototypeOption,
  EmbeddedPrototypePayload
} from '../model/types'

const {
  bakeState,
  bakeFrame,
  bakeFrameById,
  bakePrototype,
  bakeAnimation,
  prototypeOptions,
  prototypeInteractions,
  renderPrototypeFrame,
  prototypeRenderRevision,
  createPresetFrame
} = defineProps<{
  bakeState?: EmbeddedFrameBakeState
  bakeFrame?: EmbeddedFrameBake
  bakeFrameById?: EmbeddedFrameBakeById
  bakePrototype?: EmbeddedPrototypeBake
  bakeAnimation?: EmbeddedAnimatedPrototypeBake
  prototypeOptions?: EmbeddedPrototypeOption[]
  prototypeInteractions?: DevicePrototypeInteraction[]
  renderPrototypeFrame?: DevicePrototypeFrameRender
  prototypeRenderRevision?: number
  createPresetFrame?: (width: number, height: number, profileName: string) => void
}>()

type BurnMode = 'frame' | 'prototype'
type TransportMode = 'usb' | 'wifi' | 'ble' | 'wifi-live'
type UsbDisplayBackend = 'standard' | 'm5gfx'
type FrameResourceSource = 'baked' | 'uploaded' | null
type WirelessTransportMode = 'wifi' | 'ble' | 'wifi-live'
type ContentUploadMode = 'frame' | 'prototype' | 'local'
type FirmwareInitializationStatus = 'idle' | 'uploading' | 'success' | 'error'

interface FirmwareInitializationState {
  status: FirmwareInitializationStatus
  progress: number
  message: string
}

type WifiUploadContent =
  | { kind: 'frame'; payload: EmbeddedImagePayload }
  | { kind: 'prototype'; payload: EmbeddedPrototypePayload }
  | { kind: 'slideshow'; payload: WirelessImageSequencePayload }

const transportMode = ref<TransportMode>('usb')
const usbDisplayBackend = ref<UsbDisplayBackend>('standard')
const burnModeByTransport = ref<Record<TransportMode, BurnMode>>({
  usb: 'frame',
  wifi: 'frame',
  ble: 'frame',
  'wifi-live': 'frame'
})
const burnMode = computed<BurnMode>({
  get: () => burnModeByTransport.value[transportMode.value],
  set: (mode) => {
    burnModeByTransport.value[transportMode.value] = mode
  }
})
const wifiProvisionEnabled = ref(false)
const wifiSsid = ref('')
const wifiPassword = ref('')
const bleSession = useBleDeviceSession()
const serialSession = useSerialDeviceSession()
const wirelessBaseUrl = ref('http://192.168.4.1')
const wirelessStatus = ref<'idle' | 'checking' | 'uploading' | 'success' | 'error'>('idle')
const wirelessMessage = ref('连接设备后，可直接传输当前图片')
const wirelessDeviceReady = ref(false)
const wifiBaseFirmwareReady = ref(false)
const liveMirrorBusy = ref(false)
const contentUploadMode = ref<ContentUploadMode>('frame')
const framePreviewUrl = ref('')
const framePreviewPending = ref(false)
const localContentFiles = ref<File[]>([])
const localPreviewUrls = ref<string[]>([])
const localPreviewIndex = ref(0)
const localPreviewPlaying = ref(true)
const localDropActive = ref(false)
const localFileInput = ref<HTMLInputElement>()
let localPreviewTimer: number | undefined
let framePreviewRequest = 0
const activeUploadId = ref<number | null>(null)
const uploadTaskLabel = ref('')
const uploadTaskStatus = ref<'idle' | 'running' | 'success' | 'error' | 'cancelled'>('idle')
let uploadTaskSequence = 0
let activeUploadController: AbortController | null = null
const usbFlashing = ref(false)
const usbPreparing = ref(false)
const contentUploadProgress = ref(0)
const usbInitialization = ref<FirmwareInitializationState>({
  status: 'idle',
  progress: 0,
  message: ''
})
const wirelessInitialization = ref<Record<WirelessTransportMode, FirmwareInitializationState>>({
  wifi: { status: 'idle', progress: 0, message: '' },
  ble: { status: 'idle', progress: 0, message: '' },
  'wifi-live': { status: 'idle', progress: 0, message: '' }
})
const wifiLiveFirmwareRevision = ref(0)
const selectedPrototypeIds = ref<Record<TransportMode, string>>({
  usb: '',
  wifi: '',
  ble: '',
  'wifi-live': ''
})
const selectedPrototypeId = computed({
  get: () => selectedPrototypeIds.value[transportMode.value],
  set: (id: string) => {
    selectedPrototypeIds.value[transportMode.value] = id
  }
})
const wifiSequencePayload = ref<WirelessImageSequencePayload | null>(null)
const bleSequencePayload = ref<WirelessImageSequencePayload | null>(null)
const uploadedUsbFiles = ref<File[]>([])
const bakePending = ref(false)
const bakeError = ref('')
const frameResourceSources = ref<Record<TransportMode, FrameResourceSource>>({
  usb: null,
  wifi: null,
  ble: null,
  'wifi-live': null
})
const frameResourceSource = computed<FrameResourceSource>({
  get: () => frameResourceSources.value[transportMode.value],
  set: (source) => {
    frameResourceSources.value[transportMode.value] = source
  }
})
const prototypePending = ref(false)
const prototypePrepared = ref(false)
const prototypeError = ref('')
const {
  selectedProfile,
  profiles,
  imagePlacement,
  frameBackgroundColor,
  imagePayload,
  usbSequencePayload,
  prototypePayload,
  buildStatus,
  buildMessage,
  buildLog,
  manifestUrlFor,
  serviceAvailable,
  selectProfile,
  selectImage,
  selectUsbImageSequence,
  selectPrototype,
  loadCachedFirmware
} = useEmbeddedDisplay()

const resolutionLabel = computed(() => {
  const resolution = selectedProfile.value?.resolution
  return resolution ? `${resolution.width} × ${resolution.height}` : '—'
})
const imagePlacementOptions: Array<{ value: EmbeddedImagePlacement; label: string }> = [
  { value: 'stretch', label: '拉伸' },
  { value: 'contain', label: '等比缩放' },
  { value: 'pixel-perfect', label: '不缩放' }
]
const contentUploadOptions: Array<{ value: ContentUploadMode; label: string }> = [
  { value: 'frame', label: 'Frame' },
  { value: 'prototype', label: '交互' },
  { value: 'local', label: '本地' }
]
const imagePlacementSummary = computed(() => embeddedImagePlacementLabel(imagePlacement.value))
const localPreviewUrl = computed(() => localPreviewUrls.value[localPreviewIndex.value] ?? '')
const localContentLabel = computed(() => {
  if (!localContentFiles.value.length) return '图片、GIF 或 PNG 序列'
  if (localContentFiles.value.length === 1) return localContentFiles.value[0]?.name ?? '本地图片'
  return `${localContentFiles.value.length} 帧 PNG 序列 · 20 FPS`
})
const localContentPrimary = computed(() => {
  if (!localContentFiles.value.length) return '0 帧'
  if (localContentFiles.value.length === 1) return '1 个文件'
  return `${localContentFiles.value.length} 帧`
})
const localContentSecondary = computed(() => {
  const firstFileName = localContentFiles.value[0]?.name
  if (!firstFileName) return '图片、GIF 或 PNG 序列'
  return localContentFiles.value.length > 1 ? `PNG 序列 · 20 FPS · ${firstFileName}` : firstFileName
})
const availablePrototypeOptions = computed(() =>
  (prototypeOptions ?? []).filter(
    (option) => transportMode.value === 'usb' || option.contentKind !== 'animated-prototype'
  )
)
const selectedPrototype = computed(
  () =>
    availablePrototypeOptions.value.find((option) => option.id === selectedPrototypeId.value) ??
    null
)
const selectedPrototypePreview = computed(
  () =>
    prototypeInteractions?.find((interaction) => interaction.id === selectedPrototypeId.value) ??
    null
)
const selectedInteractionIsSlideshow = computed(() => selectedPrototype.value?.mode === 'slideshow')
const selectedInteractionIsAnimated = computed(
  () => selectedPrototype.value?.contentKind === 'animated-prototype'
)
const selectedInteractionModeLabel = computed(() => {
  if (selectedPrototype.value?.mode === 'slideshow') return '幻灯片'
  if (selectedPrototype.value?.mode === 'manual') return '手动浏览'
  return '自定义交互'
})
const selectedPrototypeSelectValue = computed({
  get: () => selectedPrototypeId.value || NO_PROTOTYPE_VALUE,
  set: (value: string) => {
    selectedPrototypeId.value = value === NO_PROTOTYPE_VALUE ? '' : value
  }
})
const uploadTaskRunning = computed(() => activeUploadId.value !== null)
const uploadTaskProgress = computed(() =>
  transportMode.value === 'ble' ? bleSession.progress.value : contentUploadProgress.value
)
const uploadTaskMessage = computed(() => {
  let transportMessage = buildMessage.value
  if (transportMode.value === 'ble') transportMessage = bleSession.message.value
  if (transportMode.value === 'wifi') transportMessage = wirelessMessage.value
  if (uploadTaskStatus.value === 'cancelled') return '上传已取消，可以开始新的操作'
  if (uploadTaskStatus.value === 'error') return transportMessage || '上传失败'
  if (uploadTaskStatus.value === 'success') return transportMessage || '上传完成'
  if (uploadTaskRunning.value) return transportMessage || uploadTaskLabel.value
  return transportMessage
})

function beginUploadTask(label: string): number {
  activeUploadController?.abort()
  const id = ++uploadTaskSequence
  activeUploadId.value = id
  uploadTaskLabel.value = label
  uploadTaskStatus.value = 'running'
  activeUploadController = new AbortController()
  contentUploadProgress.value = 0
  return id
}

function isUploadTaskCurrent(id: number): boolean {
  return activeUploadId.value === id
}

function finishUploadTask(id: number, status: 'success' | 'error' = 'success'): void {
  if (!isUploadTaskCurrent(id)) return
  activeUploadId.value = null
  uploadTaskStatus.value = status
  activeUploadController = null
}

function currentUploadFailed(): boolean {
  if (buildStatus.value === 'error') return true
  if (transportMode.value === 'ble') return bleSession.status.value === 'error'
  if (transportMode.value === 'wifi') return wirelessStatus.value === 'error'
  return false
}

function cancelUploadTask(): void {
  if (!uploadTaskRunning.value) return
  uploadTaskSequence += 1
  activeUploadController?.abort()
  activeUploadController = null
  activeUploadId.value = null
  uploadTaskStatus.value = 'cancelled'
  uploadTaskLabel.value = '上传已取消'
  usbFlashing.value = false
  usbPreparing.value = false
  bakePending.value = false
  prototypePending.value = false
  contentUploadProgress.value = 0
  buildStatus.value = 'idle'
  buildMessage.value = '上传已取消，可以开始新的操作'
  if (wirelessStatus.value === 'uploading' || wirelessStatus.value === 'checking') {
    wirelessStatus.value = 'idle'
  }
  if (bleSession.status.value === 'uploading' || bleSession.status.value === 'checking') {
    bleSession.cancelUpload()
  }
}

const modeSwitchLocked = computed(
  () => uploadTaskRunning.value || serialSession.selecting.value || liveMirrorBusy.value
)
const transportOptions = computed(() =>
  [
    { value: 'usb' as const, label: 'USB' },
    { value: 'wifi' as const, label: 'Wi-Fi', debugOnly: true },
    { value: 'ble' as const, label: 'BLE' },
    { value: 'wifi-live' as const, label: 'Wi-Fi 实时镜像', debugOnly: true }
  ]
    .filter((option) => !option.debugOnly || embeddedDisplayAdvancedDebugMode.value)
    .map(({ value, label }) => ({
      value,
      label,
      disabled: modeSwitchLocked.value && value !== transportMode.value
    }))
)
const transportModeLabel = computed(
  () =>
    transportOptions.value.find((option) => option.value === transportMode.value)?.label ?? '当前'
)
const firmwareActionLabel = computed(() => `写入 ${transportModeLabel.value} 模式固件`)
const M5_STOPWATCH_PROFILE_ID = 'co5300_m5stack_stopwatch'
const M5GFX_DEVICE_OPTION_ID = `${M5_STOPWATCH_PROFILE_ID}:usb-frame-m5gfx`
const profileOptions = computed(() =>
  profiles.value.flatMap((profile) => {
    const baseOption = { value: profile.id, label: profile.name }
    if (profile.id !== M5_STOPWATCH_PROFILE_ID) return [baseOption]
    return [baseOption, { value: M5GFX_DEVICE_OPTION_ID, label: `${profile.name}（M5GFX USB）` }]
  })
)
const selectedDeviceOptionId = computed(() =>
  selectedProfile.value?.id === M5_STOPWATCH_PROFILE_ID && usbDisplayBackend.value === 'm5gfx'
    ? M5GFX_DEVICE_OPTION_ID
    : selectedProfile.value?.id || ''
)
function selectDeviceOption(optionId: string): void {
  const useM5Gfx = optionId === M5GFX_DEVICE_OPTION_ID
  usbDisplayBackend.value = useM5Gfx ? 'm5gfx' : 'standard'
  selectProfile(useM5Gfx ? M5_STOPWATCH_PROFILE_ID : optionId)
}
const bleBuildMode: EmbeddedBuildMode = 'ble-frame'
const isM5StopWatch = computed(() => selectedProfile.value?.id === M5_STOPWATCH_PROFILE_ID)
const usbBuildMode = computed<UsbContentBuildMode>(() =>
  isM5StopWatch.value && usbDisplayBackend.value === 'm5gfx' ? 'usb-frame-m5gfx' : 'usb-frame'
)
const usbManifestUrl = computed(() => manifestUrlFor(usbBuildMode.value))
const bleManifestUrl = computed(() => manifestUrlFor(bleBuildMode))
const wifiManifestUrl = computed(() => manifestUrlFor('wifi-frame'))
const wifiLiveManifestUrl = computed(() => manifestUrlFor('wifi-live'))
const activeFirmwareInitialization = computed<FirmwareInitializationState>(() =>
  transportMode.value === 'usb'
    ? usbInitialization.value
    : wirelessInitialization.value[transportMode.value]
)
const activeFirmwareManifestUrl = computed(() => {
  if (transportMode.value === 'usb') return usbManifestUrl.value
  if (transportMode.value === 'ble') return bleManifestUrl.value
  if (transportMode.value === 'wifi-live') return wifiLiveManifestUrl.value
  return wifiManifestUrl.value
})
const usbFrameFastSupported = computed(() => supportsUsbFrameFastFlash(selectedProfile.value?.id))
const canBleBakeAndUpload = computed(
  () =>
    transportMode.value === 'ble' &&
    (burnMode.value === 'frame'
      ? canBake.value
      : !selectedInteractionIsAnimated.value &&
        Boolean(bakePrototype && selectedPrototype.value) &&
        prototypeReason.value === '' &&
        !prototypePending.value) &&
    (bleSession.deviceReady.value || bleSession.canReconnect.value) &&
    !['checking', 'uploading'].includes(bleSession.status.value)
)
const wifiTransferAvailable = computed(
  () =>
    transportMode.value === 'wifi' &&
    wirelessDeviceReady.value &&
    !['checking', 'uploading'].includes(wirelessStatus.value)
)
const canWifiBakeAndUpload = computed(
  () =>
    wifiTransferAvailable.value &&
    (burnMode.value === 'frame'
      ? canBake.value
      : !selectedInteractionIsAnimated.value &&
        Boolean(bakePrototype && selectedPrototype.value) &&
        prototypeReason.value === '' &&
        !prototypePending.value)
)
const canUploadCurrent = computed(
  () =>
    Boolean(selectedProfile.value && bakeState?.available) &&
    transportMode.value !== 'wifi-live' &&
    !uploadTaskRunning.value
)
const canUploadInteraction = computed(
  () =>
    Boolean(selectedProfile.value && selectedPrototype.value && selectedPrototype.value.valid) &&
    transportMode.value !== 'wifi-live' &&
    !uploadTaskRunning.value
)
const canUploadLocal = computed(
  () =>
    Boolean(selectedProfile.value && localContentFiles.value.length) &&
    transportMode.value !== 'wifi-live' &&
    !uploadTaskRunning.value
)
const canUploadSelectedContent = computed(() => {
  if (contentUploadMode.value === 'frame') return canUploadCurrent.value
  if (contentUploadMode.value === 'prototype') return canUploadInteraction.value
  return canUploadLocal.value
})
const NO_PROTOTYPE_VALUE = '__embedded-display-no-prototype__'
const prototypeSelectOptions = computed(() => [
  { value: NO_PROTOTYPE_VALUE, label: '请选择交互' },
  ...availablePrototypeOptions.value.map((option) => ({
    value: option.id,
    label: option.name
  }))
])

function handleCreatePresetFrame(): void {
  const profile = selectedProfile.value
  if (!profile || !createPresetFrame) return
  createPresetFrame(profile.resolution.width, profile.resolution.height, profile.name)
}
const bakeReason = computed(() => {
  if (!bakeState) return '请在画布中选中一个 Frame 或 Frame 内的元素'
  if (!bakeState.available) return bakeState.reason || '当前选择无法烘焙'
  if (!selectedProfile.value) return '请先选择屏幕方案'
  return bakeState.reason || ''
})
const prototypeReason = computed(() => {
  if (!selectedPrototype.value) return '请先选择一个命名交互'
  if (!selectedPrototype.value.valid) return selectedPrototype.value.reason || '交互定义不完整'
  if (!selectedProfile.value) return '请先选择屏幕方案'
  return ''
})
const canBake = computed(
  () =>
    Boolean(bakeFrame && bakeState?.available) &&
    bakeReason.value === '' &&
    !bakePending.value &&
    !['uploading', 'building'].includes(buildStatus.value)
)
const canUsbFrameFlash = computed(
  () =>
    transportMode.value === 'usb' &&
    burnMode.value === 'frame' &&
    usbFrameFastSupported.value &&
    canBake.value &&
    !usbFlashing.value &&
    !usbPreparing.value &&
    usbInitialization.value.status !== 'uploading'
)
const canUsbFileFlash = computed(
  () =>
    transportMode.value === 'usb' &&
    burnMode.value === 'frame' &&
    usbFrameFastSupported.value &&
    !usbFlashing.value &&
    !usbPreparing.value &&
    !bakePending.value &&
    usbInitialization.value.status !== 'uploading'
)
const canUsbPrototypeFlash = computed(
  () =>
    transportMode.value === 'usb' &&
    burnMode.value === 'prototype' &&
    usbFrameFastSupported.value &&
    Boolean((bakePrototype || bakeAnimation) && selectedPrototype.value) &&
    prototypeReason.value === '' &&
    !prototypePending.value &&
    !usbFlashing.value &&
    !usbPreparing.value &&
    usbInitialization.value.status !== 'uploading'
)
const wifiCredentials = computed(() =>
  wifiProvisionEnabled.value && wifiSsid.value.trim()
    ? { ssid: wifiSsid.value.trim(), password: wifiPassword.value }
    : undefined
)

watch(
  [transportMode, availablePrototypeOptions],
  ([, options]) => {
    if (!options?.some((option) => option.id === selectedPrototypeId.value)) {
      selectedPrototypeId.value = options?.[0]?.id ?? ''
    }
    prototypePrepared.value = false
  },
  { immediate: true, deep: true }
)

watch(selectedPrototypeId, () => {
  prototypePrepared.value = false
  prototypeError.value = ''
})

watch([imagePlacement, frameBackgroundColor], () => {
  prototypePrepared.value = false
  prototypeError.value = ''
})

watch(
  () => selectedProfile.value?.id,
  () => {
    prototypePrepared.value = false
    prototypeError.value = ''
    void refreshFramePreview()
  }
)

function clearLocalPreviewTimer(): void {
  if (localPreviewTimer !== undefined) window.clearInterval(localPreviewTimer)
  localPreviewTimer = undefined
}

function clearLocalPreview(): void {
  clearLocalPreviewTimer()
  for (const url of localPreviewUrls.value) URL.revokeObjectURL(url)
  localPreviewUrls.value = []
  localPreviewIndex.value = 0
}

function restartLocalPreview(): void {
  clearLocalPreviewTimer()
  localPreviewIndex.value = 0
  if (!localPreviewPlaying.value || localPreviewUrls.value.length < 2) return
  localPreviewTimer = window.setInterval(() => {
    localPreviewIndex.value = (localPreviewIndex.value + 1) % localPreviewUrls.value.length
  }, 50)
}

function setLocalContentFiles(files: File[]): void {
  const imageFiles = files.filter((file) => file.type.startsWith('image/'))
  if (!imageFiles.length) return
  clearLocalPreview()
  localContentFiles.value = imageFiles
  localPreviewUrls.value = imageFiles.map((file) => URL.createObjectURL(file))
  localPreviewPlaying.value = imageFiles.length > 1
  restartLocalPreview()
  contentUploadMode.value = 'local'
  bakeError.value = ''
}

function handleLocalContentChange(event: Event): void {
  const input = event.target as HTMLInputElement
  setLocalContentFiles([...(input.files ?? [])])
  input.value = ''
}

function handleLocalDrop(event: DragEvent): void {
  event.preventDefault()
  localDropActive.value = false
  setLocalContentFiles([...(event.dataTransfer?.files ?? [])])
}

async function refreshFramePreview(): Promise<void> {
  const request = ++framePreviewRequest
  if (!bakeFrame || !bakeState?.available || !selectedProfile.value) {
    if (framePreviewUrl.value) URL.revokeObjectURL(framePreviewUrl.value)
    framePreviewUrl.value = ''
    framePreviewPending.value = false
    return
  }
  framePreviewPending.value = true
  try {
    const file = await bakeFrame()
    if (request !== framePreviewRequest || !file) return
    if (framePreviewUrl.value) URL.revokeObjectURL(framePreviewUrl.value)
    framePreviewUrl.value = URL.createObjectURL(file)
  } catch (error) {
    if (request === framePreviewRequest)
      bakeError.value = error instanceof Error ? error.message : String(error)
  } finally {
    if (request === framePreviewRequest) framePreviewPending.value = false
  }
}

watch(
  [contentUploadMode, () => bakeState?.id, () => bakeState?.revision],
  ([mode]) => {
    if (mode === 'frame') void refreshFramePreview()
  },
  { immediate: true }
)

watch([contentUploadMode, localPreviewPlaying, () => localPreviewUrls.value.length], ([mode]) => {
  if (mode === 'local') restartLocalPreview()
  else clearLocalPreviewTimer()
})

onUnmounted(() => {
  framePreviewRequest += 1
  if (framePreviewUrl.value) URL.revokeObjectURL(framePreviewUrl.value)
  clearLocalPreview()
})

async function handleBakeFrame(taskId?: number): Promise<boolean> {
  if (!bakeFrame || !canBake.value) return false
  bakePending.value = true
  bakeError.value = ''
  try {
    const file = await bakeFrame()
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return false
    if (!file) return false
    await selectImage(file, {
      upload: false,
      placement: imagePlacement.value,
      backgroundColor: frameBackgroundColor.value
    })
    frameResourceSource.value = 'baked'
    return true
  } catch (error) {
    bakeError.value = error instanceof Error ? error.message : String(error)
    return false
  } finally {
    bakePending.value = false
  }
}

async function prepareUsbFrameContent(source: 'frame' | 'file', taskId?: number): Promise<boolean> {
  if (source === 'frame') return handleBakeFrame(taskId)
  if (!uploadedUsbFiles.value.length) return false
  const files = uploadedUsbFiles.value
  await selectImage(undefined, { upload: false })
  if (files.length === 1) {
    await selectImage(files[0], {
      upload: false,
      placement: imagePlacement.value,
      backgroundColor: frameBackgroundColor.value
    })
  } else {
    await selectUsbImageSequence(files, {
      placement: imagePlacement.value,
      backgroundColor: frameBackgroundColor.value
    })
  }
  if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return false
  return buildStatus.value !== 'error'
}

function updateUsbFirmwareStage(stage: UsbContentFirmwareStage, message: string): void {
  buildMessage.value = message
  if (stage === 'checking') contentUploadProgress.value = 3
  if (stage === 'reconnecting') contentUploadProgress.value = 65
}

async function resolveUsbFirmwareManifestUrl(): Promise<string> {
  const profileId = selectedProfile.value?.id
  if (!profileId) throw new Error('请先选择屏幕方案')

  let manifestUrl = usbManifestUrl.value
  if (!manifestUrl) {
    await loadCachedFirmware(usbBuildMode.value)
    manifestUrl = usbManifestUrl.value
  }
  if (selectedProfile.value?.id !== profileId || !manifestUrl) {
    throw new Error('USB 模式固件未就绪，请稍后重试或重新写入 USB 模式固件')
  }
  return manifestUrl
}

async function transferPreparedUsbContent(
  port: SerialPortLike,
  contentLabel: string,
  upload: (options: UsbFlashOptions) => Promise<number>,
  taskId?: number
): Promise<void> {
  const manifestUrl = await resolveUsbFirmwareManifestUrl()

  const result = await transferUsbContentWithFirmwareFallback({
    port: port as UsbContentSerialPort,
    manifestUrl,
    firmwareBuildMode: usbBuildMode.value,
    transfer: (activePort, firmwareUpdated) => {
      const progressStart = firmwareUpdated ? 70 : 10
      if (taskId !== undefined && !isUploadTaskCurrent(taskId))
        return Promise.reject(new DOMException('上传已取消', 'AbortError'))
      contentUploadProgress.value = progressStart
      return upload(
        usbTransferOptions(activePort as SerialPortLike, contentLabel, progressStart, taskId)
      )
    },
    onLog: (message) => {
      if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
      const normalized = message.trim()
      if (normalized) buildLog.value.push(normalized)
    },
    onProgress: ({ percent, written, total }) => {
      if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
      contentUploadProgress.value = 5 + Math.round(percent * 0.55)
      buildMessage.value = `检测到固件不兼容，正在自动更新：${percent}%（${written} / ${total} 字节）`
    },
    onStage: (stage, message) => {
      if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
      updateUsbFirmwareStage(stage, message)
    }
  })
  clearActiveUsbPort(result.port)
}

function usbTransferOptions(
  port: SerialPortLike,
  contentLabel: string,
  progressStart: number,
  taskId?: number
): UsbFlashOptions {
  return {
    port,
    onLog: (message) => {
      if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
      const normalized = message.trim()
      if (normalized) buildLog.value.push(normalized)
    },
    onProgress: ({ percent, written, total }) => {
      if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
      contentUploadProgress.value =
        progressStart + Math.round((percent * (100 - progressStart)) / 100)
      buildMessage.value = `正在通过 USB 高速传输${contentLabel}：${percent}%（${written} / ${total} 字节）`
    }
  }
}

interface PreparedUsbFrameContent {
  profileId: string
  label: string
  successMessage: string
  upload: (options: UsbFlashOptions) => Promise<number>
}

function preparedUsbFrameContent(source: 'frame' | 'file'): PreparedUsbFrameContent | null {
  const sequence = source === 'file' ? usbSequencePayload.value : null
  if (sequence) {
    return {
      profileId: sequence.profileId,
      label: ` PNG 序列：${sequence.frameCount} 帧`,
      successMessage: `PNG 序列已写入：${sequence.frameCount} 帧 · 20 FPS，设备正在重启。`,
      upload: (options) => flashUsbSequenceFirmware(sequence, options)
    }
  }

  const image = imagePayload.value
  if (!image) return null
  return {
    profileId: image.profileId,
    label: '单 Frame 内容',
    successMessage: '最新 Frame 已写入，设备正在重启。',
    upload: (options) => flashUsbFrameFirmware(image, options)
  }
}

async function flashPreparedUsbFrame(
  port: SerialPortLike,
  requestedProfileId: string,
  source: 'frame' | 'file',
  taskId?: number
): Promise<void> {
  const content = preparedUsbFrameContent(source)
  if (!content) {
    bakeError.value = '请先烘焙、选择图片或选择 PNG 序列'
    return
  }
  if (
    transportMode.value !== 'usb' ||
    burnMode.value !== 'frame' ||
    selectedProfile.value?.id !== requestedProfileId ||
    content.profileId !== requestedProfileId
  ) {
    return
  }

  usbFlashing.value = true
  contentUploadProgress.value = 0
  buildStatus.value = 'uploading'
  buildMessage.value = `正在准备 USB ${content.label}…`
  buildLog.value = []
  try {
    await transferPreparedUsbContent(port, content.label, content.upload, taskId)
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
    contentUploadProgress.value = 100
    buildStatus.value = 'ready'
    buildMessage.value = content.successMessage
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    buildStatus.value = 'error'
    buildMessage.value = `USB Frame 传输失败：${message}`
    buildLog.value.push(message)
  } finally {
    usbFlashing.value = false
  }
}

async function handleUsbFrameBakeAndFlash(source: 'frame' | 'file' = 'frame', taskId?: number) {
  const requestedProfileId = selectedProfile.value?.id
  const canStart = source === 'frame' ? canUsbFrameFlash.value : canUsbFileFlash.value
  if (!requestedProfileId || !canStart) return

  usbPreparing.value = true
  try {
    const port = await serialSession.requirePort()
    if (!(await prepareUsbFrameContent(source, taskId))) return
    await flashPreparedUsbFrame(port, requestedProfileId, source, taskId)
  } catch (error) {
    bakeError.value = error instanceof Error ? error.message : String(error)
  } finally {
    usbPreparing.value = false
  }
}

async function handleUsbPrototypeBakeAndFlash(taskId?: number) {
  if (!canUsbPrototypeFlash.value || usbPreparing.value) return
  const requestedProfileId = selectedProfile.value?.id
  if (!requestedProfileId) return

  usbPreparing.value = true
  try {
    const port = await serialSession.requirePort()
    const animated = selectedInteractionIsAnimated.value
    const selectedInteraction = selectedPrototype.value
    if (!selectedInteraction) return
    let interactionPayload:
      | EmbeddedPrototypePayload
      | WirelessImageSequencePayload
      | EmbeddedAnimatedPrototypeBakeResult
      | null
    if (animated) {
      interactionPayload = bakeAnimation?.(selectedInteraction.id) ?? null
    } else {
      if (!(await preparePrototypeResources(false, taskId))) return
      interactionPayload = selectedInteractionIsSlideshow.value
        ? usbSequencePayload.value
        : prototypePayload.value
    }
    if (!interactionPayload) return
    if (
      transportMode.value !== 'usb' ||
      burnMode.value !== 'prototype' ||
      selectedProfile.value?.id !== requestedProfileId ||
      (!animated &&
        !('states' in interactionPayload) &&
        interactionPayload.profileId !== requestedProfileId)
    ) {
      return
    }

    usbFlashing.value = true
    contentUploadProgress.value = 0
    buildStatus.value = 'uploading'
    buildMessage.value = `正在准备 USB ${selectedInteractionModeLabel.value}内容…`
    buildLog.value = []
    let upload: (options: UsbFlashOptions) => Promise<number>
    if (animated && 'states' in interactionPayload) {
      const animationBake = interactionPayload as EmbeddedAnimatedPrototypeBakeResult
      const payload = await encodeUsbAnimatedPrototype(
        animationBake,
        selectedProfile.value,
        imagePlacement.value,
        frameBackgroundColor.value
      )
      upload = (options) => flashUsbAnimatedPrototypeFirmware(payload, options)
    } else if (selectedInteractionIsSlideshow.value && usbSequencePayload.value) {
      const sequence = usbSequencePayload.value
      upload = (options) => flashUsbSequenceFirmware(sequence, options)
    } else if (prototypePayload.value) {
      const prototype = prototypePayload.value
      upload = (options) => flashUsbPrototypeFirmware(prototype, options)
    } else throw new Error('交互内容尚未准备完成')
    await transferPreparedUsbContent(port, selectedInteractionModeLabel.value, upload, taskId)
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
    contentUploadProgress.value = 100
    buildStatus.value = 'ready'
    buildMessage.value = `${selectedInteractionModeLabel.value}和全部画面已写入，设备正在重启。`
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    prototypeError.value = message
    buildStatus.value = 'error'
    buildMessage.value = `USB 交互传输失败：${message}`
    buildLog.value.push(message)
  } finally {
    if (usbFlashing.value) usbFlashing.value = false
    usbPreparing.value = false
  }
}

async function handleBleBakeAndUpload(taskId?: number) {
  if (!canBleBakeAndUpload.value && taskId === undefined) return
  bleSequencePayload.value = null
  const requestedMode = burnMode.value
  const requestedProfileId = selectedProfile.value?.id
  if (!requestedProfileId) return

  if (!(await ensureBleUploadDevice(selectedProfile.value, requestedMode, taskId))) return

  if (requestedMode === 'prototype') {
    if (!(await preparePrototypeResources(false, taskId))) return
    if (
      transportMode.value !== 'ble' ||
      burnMode.value !== requestedMode ||
      selectedProfile.value?.id !== requestedProfileId
    ) {
      return
    }
    const interactionPayload = selectedInteractionIsSlideshow.value
      ? bleSequencePayload.value
      : prototypePayload.value
    if (!interactionPayload) return
    await bleSession.upload(interactionPayload, selectedProfile.value)
    return
  }

  if (!(await handleBakeFrame(taskId)) || !imagePayload.value) return
  if (
    transportMode.value !== 'ble' ||
    burnMode.value !== requestedMode ||
    selectedProfile.value?.id !== requestedProfileId
  ) {
    return
  }
  await bleSession.upload(imagePayload.value, selectedProfile.value)
}

async function handleWifiBakeAndUpload(taskId?: number) {
  if (!wirelessDeviceReady.value) await handleProbeWifi()
  if (!wirelessDeviceReady.value || (taskId !== undefined && !isUploadTaskCurrent(taskId))) return
  if (!canWifiBakeAndUpload.value && taskId === undefined) return
  wifiSequencePayload.value = null
  const requestedMode = burnMode.value
  const requestedProfileId = selectedProfile.value?.id
  if (!requestedProfileId) return

  if (requestedMode === 'prototype') {
    if (!(await preparePrototypeResources(false, taskId))) return
  } else if (!(await handleBakeFrame(taskId)) || !imagePayload.value) {
    return
  }

  await uploadWifiContent(requestedMode, requestedProfileId, taskId)
}

async function ensureBleUploadDevice(
  profile: NonNullable<typeof selectedProfile.value>,
  mode: BurnMode,
  taskId?: number
): Promise<boolean> {
  if (bleSession.deviceReady.value || bleSession.canReconnect.value) return true
  buildMessage.value = '请选择要上传到的 BLE 设备'
  const connection = await bleSession.probe(profile, mode)
  if (!connection || !bleSession.deviceReady.value) return false
  return taskId === undefined || isUploadTaskCurrent(taskId)
}

async function handleUploadSelectedContent(): Promise<void> {
  if (contentUploadMode.value === 'frame') {
    await handleUploadCurrentFrame()
    return
  }
  if (contentUploadMode.value === 'prototype') {
    await handleUploadInteraction()
    return
  }
  await handleUploadLocalContent()
}

async function handleUploadCurrentFrame(): Promise<void> {
  const profile = selectedProfile.value
  if (!profile || !bakeState?.available) {
    bakeError.value = bakeReason.value
    return
  }
  burnMode.value = 'frame'
  const taskId = beginUploadTask('上传当前 Frame')
  try {
    if (transportMode.value === 'usb') await handleUsbFrameBakeAndFlash('frame', taskId)
    else if (transportMode.value === 'ble') await handleBleBakeAndUpload(taskId)
    else if (transportMode.value === 'wifi') await handleWifiBakeAndUpload(taskId)
    if (isUploadTaskCurrent(taskId)) {
      finishUploadTask(taskId, currentUploadFailed() ? 'error' : 'success')
    }
  } catch (error) {
    if (!isUploadTaskCurrent(taskId)) return
    const message = error instanceof Error ? error.message : String(error)
    buildStatus.value = 'error'
    buildMessage.value = message
    finishUploadTask(taskId, 'error')
  }
}

async function handleUploadInteraction(): Promise<void> {
  if (!selectedProfile.value || !selectedPrototype.value) {
    prototypeError.value = prototypeReason.value
    return
  }
  burnMode.value = 'prototype'
  const taskId = beginUploadTask(`上传交互：${selectedPrototype.value.name}`)
  try {
    if (transportMode.value === 'usb') await handleUsbPrototypeBakeAndFlash(taskId)
    else if (transportMode.value === 'ble') await handleBleBakeAndUpload(taskId)
    else if (transportMode.value === 'wifi') await handleWifiBakeAndUpload(taskId)
    if (isUploadTaskCurrent(taskId)) {
      finishUploadTask(taskId, currentUploadFailed() ? 'error' : 'success')
    }
  } catch (error) {
    if (!isUploadTaskCurrent(taskId)) return
    const message = error instanceof Error ? error.message : String(error)
    prototypeError.value = message
    buildStatus.value = 'error'
    buildMessage.value = message
    finishUploadTask(taskId, 'error')
  }
}

async function handleUploadLocalContent(): Promise<void> {
  const files = [...localContentFiles.value]
  const profile = selectedProfile.value
  if (!files.length || !profile) return

  burnMode.value = 'frame'
  const taskId = beginUploadTask('上传本地内容')
  try {
    if (transportMode.value === 'usb') {
      const port = await serialSession.requirePort()
      if (!isUploadTaskCurrent(taskId)) return
      uploadedUsbFiles.value = files
      if (!(await prepareUsbFrameContent('file', taskId))) {
        throw new Error(bakeError.value || buildMessage.value || '本地内容准备失败')
      }
      await flashPreparedUsbFrame(port, profile.id, 'file', taskId)
    } else if (transportMode.value === 'ble') {
      if (!(await ensureBleUploadDevice(profile, 'frame', taskId))) {
        throw new Error(bleSession.message.value || '未找到可用 BLE 设备')
      }
      await uploadBleLocalContent(files, profile.id, taskId)
    } else if (transportMode.value === 'wifi') {
      if (!wirelessDeviceReady.value) await handleProbeWifi()
      if (!wirelessDeviceReady.value || !isUploadTaskCurrent(taskId)) {
        throw new Error(wirelessMessage.value || '未找到可用 Wi-Fi 设备')
      }
      await uploadWifiLocalContent(files, profile.id, taskId)
    }
    if (isUploadTaskCurrent(taskId)) {
      finishUploadTask(taskId, currentUploadFailed() ? 'error' : 'success')
    }
  } catch (error) {
    if (!isUploadTaskCurrent(taskId)) return
    const message = error instanceof Error ? error.message : String(error)
    bakeError.value = message
    buildStatus.value = 'error'
    buildMessage.value = message
    finishUploadTask(taskId, 'error')
  }
}

async function uploadWifiLocalContent(
  files: File[],
  requestedProfileId: string,
  taskId: number
): Promise<void> {
  const profile = selectedProfile.value
  if (!profile || profile.id !== requestedProfileId) return

  frameResourceSource.value = 'uploaded'
  wifiSequencePayload.value = null
  await selectImage(undefined, { upload: false })
  if (files.length === 1) {
    await selectImage(files[0], {
      upload: false,
      placement: imagePlacement.value,
      backgroundColor: frameBackgroundColor.value
    })
  } else {
    wifiSequencePayload.value = await imageFilesToWifiSequence(files, profile, {
      placement: imagePlacement.value,
      backgroundColor: frameBackgroundColor.value
    })
  }
  if (!isUploadTaskCurrent(taskId) || selectedProfile.value?.id !== requestedProfileId) return
  await uploadWifiContent('frame', requestedProfileId, taskId)
}

async function uploadBleLocalContent(
  files: File[],
  requestedProfileId: string,
  taskId: number
): Promise<void> {
  const profile = selectedProfile.value
  if (!profile || profile.id !== requestedProfileId) return

  frameResourceSource.value = 'uploaded'
  bleSequencePayload.value = null
  await selectImage(undefined, { upload: false })
  if (files.length === 1) {
    await selectImage(files[0], {
      upload: false,
      placement: imagePlacement.value,
      backgroundColor: frameBackgroundColor.value
    })
    if (
      !isUploadTaskCurrent(taskId) ||
      !isWirelessSingleImagePayload(imagePayload.value, requestedProfileId)
    ) {
      return
    }
    await bleSession.upload(imagePayload.value, profile)
    return
  }

  const sequence = await imageFilesToBleSequence(files, profile, {
    placement: imagePlacement.value,
    backgroundColor: frameBackgroundColor.value
  })
  if (!isUploadTaskCurrent(taskId) || selectedProfile.value?.id !== requestedProfileId) return
  bleSequencePayload.value = sequence
  await bleSession.upload(sequence, profile)
}

async function preparePrototypeResources(uploadToBuildService = false, taskId?: number) {
  if (!bakePrototype || !selectedPrototype.value || prototypeReason.value) return false
  prototypePending.value = true
  prototypePrepared.value = false
  prototypeError.value = ''
  try {
    const bake = await bakePrototype(selectedPrototypeId.value)
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return false
    if (!bake) throw new Error('无法读取所选交互')
    usbSequencePayload.value = null
    wifiSequencePayload.value = null
    bleSequencePayload.value = null
    prototypePayload.value = null
    if (bake.mode === 'slideshow') {
      const profile = selectedProfile.value
      if (!profile) throw new Error('请先选择屏幕方案')
      const files = bake.states.map((state) => state.file)
      const options = {
        frameDelayMs: bake.intervalMs,
        preserveOrder: true,
        placement: imagePlacement.value,
        backgroundColor: frameBackgroundColor.value
      }
      if (transportMode.value === 'usb') {
        usbSequencePayload.value = await imageFilesToUsbSequence(files, profile, options)
      } else if (transportMode.value === 'wifi') {
        wifiSequencePayload.value = await imageFilesToWifiSequence(files, profile, options)
      } else if (transportMode.value === 'ble') {
        bleSequencePayload.value = await imageFilesToBleSequence(files, profile, options)
      }
      buildStatus.value = 'idle'
      buildMessage.value = `幻灯片已准备：${files.length} 个画面 · 每 ${(bake.intervalMs / 1000).toFixed(1)} 秒切换`
      buildLog.value = [
        `slideshow: ${bake.name}`,
        `states: ${files.length}`,
        `interval: ${bake.intervalMs} ms`,
        'sequence-payload: ready'
      ]
    } else {
      await selectPrototype(bake, {
        upload: uploadToBuildService,
        backgroundColor: frameBackgroundColor.value,
        placement: imagePlacement.value
      })
    }
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return false
    prototypePrepared.value = true
    return true
  } catch (error) {
    prototypeError.value = error instanceof Error ? error.message : String(error)
    return false
  } finally {
    prototypePending.value = false
  }
}

function resetWirelessInitialization(mode: WirelessTransportMode) {
  wirelessInitialization.value[mode] = {
    status: 'idle',
    progress: 0,
    message: ''
  }
}

function resetUsbInitialization() {
  usbInitialization.value = { status: 'idle', progress: 0, message: '' }
}

async function handleInitializeUsbFirmware(taskId?: number) {
  const manifestUrl = usbManifestUrl.value
  const profileId = selectedProfile.value?.id
  if (
    transportMode.value !== 'usb' ||
    !manifestUrl ||
    !profileId ||
    usbFlashing.value ||
    usbPreparing.value ||
    usbInitialization.value.status === 'uploading'
  ) {
    return
  }

  const state = usbInitialization.value
  let port
  try {
    port = await serialSession.requirePort()
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    state.status = 'error'
    state.message = message
    buildStatus.value = 'error'
    buildMessage.value = message
    return
  }

  state.status = 'uploading'
  state.progress = 0
  state.message = '正在准备 USB 模式固件…'
  buildStatus.value = 'uploading'
  buildMessage.value = state.message
  buildLog.value = []

  try {
    await withUsbDeploymentLock(() =>
      flashFirmwareManifest(manifestUrl, usbBuildMode.value, {
        port,
        preparingMessage: state.message,
        connectedMessage: '已连接，正在写入 USB 高速传输固件。',
        onLog: (message) => {
          if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
          const normalized = message.trim()
          if (!normalized) return
          state.message = normalized
          buildMessage.value = normalized
          buildLog.value.push(normalized)
        },
        onProgress: ({ percent, written, total }) => {
          if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
          state.progress = percent
          state.message = `正在写入 USB 模式固件：${percent}%（${written} / ${total} 字节）`
          buildMessage.value = state.message
        }
      })
    )
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
    clearActiveUsbPort(port as UsbContentSerialPort)
    state.status = 'success'
    state.progress = 100
    state.message = 'USB 模式固件已写入；可以传输内容。'
    buildStatus.value = 'ready'
    buildMessage.value = state.message
  } catch (error) {
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
    const message = error instanceof Error ? error.message : String(error)
    state.status = 'error'
    state.message = `USB 模式固件写入失败：${message}`
    buildStatus.value = 'error'
    buildMessage.value = state.message
    buildLog.value.push(message)
  }
}

async function handleInitializeFirmware(taskId?: number) {
  const mode = transportMode.value
  if (mode === 'usb') {
    await handleInitializeUsbFirmware(taskId)
    return
  }
  await handleInitializeWirelessFirmware(mode, taskId)
}

async function handleInitializeWirelessFirmware(mode: WirelessTransportMode, taskId?: number) {
  if (transportMode.value !== mode) return
  const { manifestUrl, modeLabel, buildMode } = wirelessFirmwareConfiguration(mode)
  const profileId = selectedProfile.value?.id
  if (!manifestUrl || !profileId) return

  const state = wirelessInitialization.value[mode]
  let port
  try {
    port = await serialSession.requirePort()
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    state.status = 'error'
    state.message = message
    buildStatus.value = 'error'
    buildMessage.value = message
    return
  }

  state.status = 'uploading'
  state.progress = 0
  state.message = `正在准备 ${modeLabel} 模式固件…`
  buildStatus.value = 'uploading'
  buildMessage.value = state.message
  buildLog.value = []

  try {
    let firmwareManifestUrl = manifestUrl
    if (mode !== 'ble') {
      state.message = `正在准备 ${modeLabel} 配置…`
      buildMessage.value = state.message
      firmwareManifestUrl =
        (await prepareWifiFirmwareCredentials(
          profileId,
          wifiCredentials.value,
          mode === 'wifi-live' ? 'wifi-live' : 'wifi-frame'
        )) || manifestUrl
    }
    await flashFirmwareManifest(firmwareManifestUrl, buildMode, {
      port,
      preparingMessage: state.message,
      connectedMessage: `已连接，正在写入 ${modeLabel} 模式固件。`,
      onLog: (message) => {
        if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
        const normalized = message.trim()
        if (!normalized) return
        state.message = normalized
        buildMessage.value = normalized
        buildLog.value.push(normalized)
      },
      onProgress: ({ percent, written, total }) => {
        if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
        state.progress = percent
        state.message = `正在写入 ${modeLabel} 模式固件：${percent}%（${written} / ${total} 字节）`
        buildMessage.value = state.message
      }
    })
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
    state.status = 'success'
    state.progress = 100
    state.message = `${modeLabel} 模式固件已写入，设备正在重启。`
    buildStatus.value = 'ready'
    buildMessage.value = state.message
    if (mode === 'wifi') {
      wifiBaseFirmwareReady.value = true
      wirelessDeviceReady.value = false
      wirelessStatus.value = 'idle'
      wirelessMessage.value = '初始化完成；连接设备热点后检查连接'
    } else if (mode === 'ble') {
      bleSession.markFirmwareBuilt('BLE 初始化完成；设备重启后可直接连接')
    } else {
      wifiBaseFirmwareReady.value = true
      wifiLiveFirmwareRevision.value += 1
    }
  } catch (error) {
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
    const message = error instanceof Error ? error.message : String(error)
    state.status = 'error'
    state.message = `${modeLabel} 模式固件写入失败：${message}`
    buildStatus.value = 'error'
    buildMessage.value = state.message
    buildLog.value.push(message)
  }
}

async function handleInitializeFirmwareTask(): Promise<void> {
  if (!activeFirmwareManifestUrl.value || uploadTaskRunning.value) return
  const taskId = beginUploadTask(firmwareActionLabel.value)
  try {
    await handleInitializeFirmware(taskId)
    if (isUploadTaskCurrent(taskId)) {
      const failed = activeFirmwareInitialization.value.status === 'error'
      finishUploadTask(taskId, failed ? 'error' : 'success')
    }
  } catch (error) {
    if (!isUploadTaskCurrent(taskId)) return
    const message = error instanceof Error ? error.message : String(error)
    buildStatus.value = 'error'
    buildMessage.value = message
    finishUploadTask(taskId, 'error')
  }
}

function wirelessFirmwareConfiguration(mode: WirelessTransportMode): {
  manifestUrl: string
  modeLabel: string
  buildMode: Extract<EmbeddedBuildMode, 'wifi-frame' | 'wifi-live' | 'ble-frame'>
} {
  if (mode === 'ble') {
    return { manifestUrl: bleManifestUrl.value, modeLabel: 'BLE', buildMode: 'ble-frame' }
  }
  if (mode === 'wifi-live') {
    return {
      manifestUrl: wifiLiveManifestUrl.value,
      modeLabel: 'Wi-Fi 实时镜像',
      buildMode: 'wifi-live'
    }
  }
  return { manifestUrl: wifiManifestUrl.value, modeLabel: 'Wi-Fi', buildMode: 'wifi-frame' }
}

async function handleProbeWifi() {
  const requestedProfile = selectedProfile.value
  const requestedBaseUrl = wirelessBaseUrl.value
  if (!requestedProfile || transportMode.value !== 'wifi') return

  wirelessStatus.value = 'checking'
  wirelessMessage.value = '正在检查设备连接…'
  buildLog.value = [`wifi-device: ${requestedBaseUrl}`, 'probe: checking']
  try {
    const device = await probeWirelessDevice(requestedBaseUrl)
    if (transportMode.value !== 'wifi' || selectedProfile.value?.id !== requestedProfile.id) {
      return
    }
    if (
      device.width !== requestedProfile.resolution.width ||
      device.height !== requestedProfile.resolution.height
    ) {
      throw new Error(
        `设备分辨率为 ${device.width} × ${device.height}，与当前方案 ${requestedProfile.resolution.width} × ${requestedProfile.resolution.height} 不匹配`
      )
    }
    wirelessDeviceReady.value = true
    wifiBaseFirmwareReady.value = true
    wirelessStatus.value = 'success'
    wirelessMessage.value = `设备已连接：${device.width} × ${device.height}${device.ip ? `，Wi-Fi 地址 ${device.ip}` : ''}`
    buildLog.value = [
      `wifi-device: ${requestedBaseUrl}`,
      `size: ${device.width}×${device.height}`,
      'probe: ok'
    ]
  } catch (error) {
    if (transportMode.value !== 'wifi' || selectedProfile.value?.id !== requestedProfile.id) {
      return
    }
    const message = error instanceof Error ? error.message : String(error)
    wirelessStatus.value = 'error'
    wirelessMessage.value = message
    buildLog.value = [`wifi-device: ${requestedBaseUrl}`, `probe-error: ${message}`]
  }
}

function preparedWifiContent(requestedMode: 'frame' | 'prototype'): WifiUploadContent | null {
  if (requestedMode === 'frame') {
    if (wifiSequencePayload.value) {
      return { kind: 'slideshow', payload: wifiSequencePayload.value }
    }
    return imagePayload.value ? { kind: 'frame', payload: imagePayload.value } : null
  }
  if (selectedInteractionIsSlideshow.value) {
    return wifiSequencePayload.value
      ? { kind: 'slideshow', payload: wifiSequencePayload.value }
      : null
  }
  return prototypePayload.value ? { kind: 'prototype', payload: prototypePayload.value } : null
}

async function sendWifiContent(
  baseUrl: string,
  content: WifiUploadContent,
  onProgress: (progress: { percent: number }) => void,
  signal?: AbortSignal
): Promise<void> {
  if (content.kind === 'frame') {
    await uploadWirelessImage(baseUrl, content.payload, signal, onProgress)
    return
  }
  if (content.kind === 'slideshow') {
    await uploadWirelessSequence(baseUrl, content.payload, signal, onProgress)
    return
  }
  await uploadWirelessPrototype(baseUrl, content.payload, signal, onProgress)
}

async function uploadWifiContent(
  requestedMode: 'frame' | 'prototype',
  requestedProfileId: string,
  taskId?: number
) {
  if (
    transportMode.value !== 'wifi' ||
    burnMode.value !== requestedMode ||
    selectedProfile.value?.id !== requestedProfileId ||
    !wifiTransferAvailable.value
  ) {
    return
  }

  const requestedBaseUrl = wirelessBaseUrl.value
  const content = preparedWifiContent(requestedMode)
  if (!content) return

  wirelessStatus.value = 'uploading'
  contentUploadProgress.value = 0
  wirelessMessage.value =
    requestedMode === 'prototype'
      ? `正在通过 Wi-Fi 传输${selectedInteractionModeLabel.value}…`
      : '正在通过 Wi-Fi 传输图片…'
  buildLog.value = [
    `wifi-device: ${requestedBaseUrl}`,
    `content: ${requestedMode}`,
    'upload: sending'
  ]
  try {
    const onProgress = ({ percent }: { percent: number }) => {
      contentUploadProgress.value = percent
    }
    await sendWifiContent(requestedBaseUrl, content, onProgress, activeUploadController?.signal)
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
    contentUploadProgress.value = 100
    wirelessStatus.value = 'success'
    wirelessMessage.value =
      requestedMode === 'prototype'
        ? `${selectedInteractionModeLabel.value}已传输，设备将重启并加载交互内容`
        : '图片已传输，设备将重启并加载新内容'
    buildLog.value.push('upload: ok')
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    if (taskId !== undefined && !isUploadTaskCurrent(taskId)) return
    wirelessStatus.value = 'error'
    wirelessMessage.value = message
    buildLog.value.push(`upload-error: ${message}`)
  }
}

function buildMessageForTransport(mode: TransportMode): string {
  if (mode === 'usb') return '选择内容后可直接烧录。'
  if (mode === 'wifi') return '连接 Wi-Fi 设备后可传输内容。'
  if (mode === 'wifi-live') return '连接 Wi-Fi 设备后可开始实时镜像。'
  return bleSession.message.value
}

let firmwareLoadSequence = 0

function resetTransportFirmwareState(mode: TransportMode): void {
  wirelessDeviceReady.value = false
  wifiBaseFirmwareReady.value = false
  bakeError.value = ''
  frameResourceSource.value = null
  prototypeError.value = ''
  prototypePrepared.value = false
  buildLog.value = []
  if (buildStatus.value !== 'loading') buildStatus.value = 'idle'
  buildMessage.value = buildMessageForTransport(mode)
  if (mode === 'usb') resetUsbInitialization()
  if (mode === 'wifi') {
    resetWirelessInitialization('wifi')
    wirelessStatus.value = 'idle'
    wirelessMessage.value = '连接设备热点后检查连接，再传输当前模式的内容'
  }
  if (mode === 'ble') {
    resetWirelessInitialization('ble')
    bleSession.setProfile(selectedProfile.value)
  }
  if (mode === 'wifi-live') resetWirelessInitialization('wifi-live')
}

async function loadTransportFirmware(
  mode: TransportMode,
  profileId: string,
  sequence: number
): Promise<void> {
  if (mode === 'usb') {
    await loadCachedFirmware(usbBuildMode.value)
    return
  }
  if (mode === 'ble') {
    const available = await loadCachedFirmware(bleBuildMode)
    if (
      sequence === firmwareLoadSequence &&
      transportMode.value === 'ble' &&
      selectedProfile.value?.id === profileId
    ) {
      bleSession.setBaseFirmwareReady(available)
    }
    return
  }
  if (mode !== 'wifi' && mode !== 'wifi-live') return
  const available = await loadCachedFirmware(mode === 'wifi-live' ? 'wifi-live' : 'wifi-frame')
  if (sequence === firmwareLoadSequence && transportMode.value === mode) {
    wifiBaseFirmwareReady.value = available
  }
}

watch(
  [transportMode, () => selectedProfile.value?.id, usbBuildMode],
  async ([mode, profileId]) => {
    const sequence = ++firmwareLoadSequence
    resetTransportFirmwareState(mode)
    if (!profileId) return
    await loadTransportFirmware(mode, profileId, sequence)
  },
  { immediate: true }
)

watch(
  () => selectedProfile.value?.id,
  (profileId) => {
    if (profileId !== M5_STOPWATCH_PROFILE_ID) usbDisplayBackend.value = 'standard'
  },
  { immediate: true }
)

watch(embeddedDisplayAdvancedDebugMode, (enabled) => {
  if (!enabled && (transportMode.value === 'wifi' || transportMode.value === 'wifi-live')) {
    transportMode.value = 'usb'
  }
})

watch([wifiSsid, wifiPassword], () => {
  wifiBaseFirmwareReady.value = false
  if (wirelessInitialization.value.wifi.status !== 'uploading') {
    resetWirelessInitialization('wifi')
  }
  if (wirelessInitialization.value['wifi-live'].status !== 'uploading') {
    resetWirelessInitialization('wifi-live')
  }
})
</script>
<template>
  <div class="flex min-h-0 flex-1 flex-col bg-panel text-surface">
    <PanelHeader>
      <template #icon>
        <icon-lucide-cpu class="size-panel-icon" />
      </template>
      <span role="heading" aria-level="2">设备烧录</span>
      <template #actions>
        <span
          class="flex items-center gap-1 text-[10px]"
          :class="serviceAvailable ? 'text-success' : 'text-muted'"
        >
          <span class="size-1.5 rounded-full bg-current" />
          {{ serviceAvailable ? '就绪' : '离线' }}
        </span>
      </template>
    </PanelHeader>

    <div class="scrollbar-thin min-h-0 flex-1 overflow-x-hidden overflow-y-auto pb-3">
      <PanelSection v-if="selectedProfile" label="固件烧录" :default-open="true">
        <AppSelect
          v-if="profiles.length"
          :model-value="selectedDeviceOptionId"
          :options="profileOptions"
          :disabled="modeSwitchLocked"
          label="设备型号"
          @update:model-value="selectDeviceOption"
        />
        <div class="mt-2 flex min-w-0 items-center gap-2">
          <SegmentedControl
            v-model="transportMode"
            class="min-w-0 flex-1"
            :options="transportOptions"
            label="选择传输方式"
          />
          <button
            type="button"
            class="flex h-control shrink-0 items-center gap-1.5 whitespace-nowrap rounded-panel border border-border bg-canvas px-3 text-[10px] font-medium text-surface hover:bg-hover disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="uploadTaskRunning || !activeFirmwareManifestUrl"
            :title="
              serialSession.ready.value ? firmwareActionLabel : `选择串口并${firmwareActionLabel}`
            "
            @click="handleInitializeFirmwareTask"
          >
            <icon-lucide-download class="size-3.5" />
            <span v-if="uploadTaskRunning && activeFirmwareInitialization.status === 'uploading'">
              {{ activeFirmwareInitialization.progress }}%
            </span>
            <span v-else>固件</span>
          </button>
        </div>
      </PanelSection>

      <PanelSection v-if="selectedProfile" label="画面设置" :default-open="true">
        <div class="flex items-center gap-2">
          <SegmentedControl
            v-model="imagePlacement"
            class="min-w-0 flex-1"
            :options="imagePlacementOptions"
            label="选择画面适配方式"
          />
          <label
            class="flex size-control shrink-0 cursor-pointer items-center justify-center rounded-panel border border-border bg-canvas"
            :title="`补边颜色 ${frameBackgroundColor.toUpperCase()}`"
          >
            <span
              class="size-4 rounded border border-border"
              :style="{ backgroundColor: frameBackgroundColor }"
            />
            <input
              v-model="frameBackgroundColor"
              type="color"
              aria-label="画面补边颜色"
              class="sr-only"
            />
          </label>
        </div>
        <p class="mt-1.5 truncate text-[10px] text-muted">
          {{ imagePlacementSummary }} · {{ frameBackgroundColor.toUpperCase() }} ·
          {{ resolutionLabel }}
        </p>
      </PanelSection>

      <PanelSection v-if="transportMode !== 'wifi-live'" label="上传内容" :default-open="true">
        <div class="flex min-w-0 items-center gap-2">
          <SegmentedControl
            v-model="contentUploadMode"
            class="min-w-0 flex-1"
            :options="contentUploadOptions"
            label="选择内容类型"
          />
          <button
            type="button"
            class="flex h-control shrink-0 items-center gap-1.5 rounded-panel bg-accent px-3 text-[11px] font-medium text-white hover:brightness-110 disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="!canUploadSelectedContent"
            title="上传当前选中的内容"
            @click="handleUploadSelectedContent"
          >
            <icon-lucide-upload class="size-3.5" />
            上传
          </button>
        </div>

        <div class="mt-2 min-w-0">
          <template v-if="contentUploadMode === 'frame'">
            <div
              data-test-id="embedded-content-stage-frame"
              class="min-w-0 overflow-hidden rounded-panel border border-border bg-panel-field"
            >
              <div class="flex h-52 items-center justify-center p-2">
                <EmbeddedDisplayContentPreview
                  v-if="framePreviewUrl"
                  :src="framePreviewUrl"
                  :alt="bakeState?.name || '当前 Frame'"
                  :placement="imagePlacement"
                  :background-color="frameBackgroundColor"
                  :target-width="selectedProfile?.resolution.width || 1"
                  :target-height="selectedProfile?.resolution.height || 1"
                  :source-width="bakeState?.width"
                  :source-height="bakeState?.height"
                  :round="selectedProfile?.visibleArea?.shape === 'round'"
                  class="w-[min(76%,192px)] max-w-full"
                />
                <div
                  v-else
                  class="flex size-44 max-w-[76%] items-center justify-center rounded-panel border border-dashed border-border text-muted"
                >
                  <icon-lucide-frame class="size-6" />
                </div>
              </div>
              <div class="grid h-24 min-w-0 grid-rows-3 border-t border-border px-3">
                <p class="flex min-w-0 items-center truncate text-[11px] font-medium text-surface">
                  {{ bakeState?.name || '当前 Frame' }}
                </p>
                <p class="flex min-w-0 items-center truncate text-[9px] text-muted">
                  <template v-if="bakeState?.available">
                    {{ bakeState.width }} × {{ bakeState.height }} · {{ imagePlacementSummary }}
                  </template>
                  <template v-else>
                    {{ framePreviewPending ? '正在生成预览…' : bakeReason }}
                  </template>
                </p>
                <div class="flex min-w-0 items-center gap-1.5">
                  <button
                    type="button"
                    class="flex h-7 min-w-0 items-center gap-1.5 rounded-panel border border-border bg-canvas px-2 text-[9px] text-surface hover:bg-hover disabled:cursor-not-allowed disabled:opacity-50"
                    :disabled="!createPresetFrame || !selectedProfile"
                    title="按当前设备屏幕尺寸在画布中创建 Frame"
                    @click="handleCreatePresetFrame"
                  >
                    <icon-lucide-square-plus class="size-3.5 shrink-0" />
                    <span class="truncate">创建预设 Frame</span>
                  </button>
                </div>
              </div>
            </div>
          </template>

          <template v-else-if="contentUploadMode === 'prototype'">
            <div v-if="renderPrototypeFrame && selectedProfile">
              <DevicePrototypePreview
                :open="true"
                :inline="true"
                :interaction="selectedPrototypePreview"
                :render-frame="renderPrototypeFrame"
                :render-revision="prototypeRenderRevision"
                :profile="selectedProfile"
                :placement="imagePlacement"
                :background-color="frameBackgroundColor"
              >
                <template #controls>
                  <AppSelect
                    v-model="selectedPrototypeSelectValue"
                    class="w-full"
                    :options="prototypeSelectOptions"
                    :disabled="uploadTaskRunning"
                    label="选择要上传的交互"
                  />
                </template>
              </DevicePrototypePreview>
            </div>
            <div
              v-else
              class="flex h-[306px] items-center justify-center rounded-panel border border-dashed border-border bg-panel-field text-[10px] text-muted"
            >
              请选择一个可预览的交互
            </div>
          </template>

          <template v-else>
            <div
              data-test-id="embedded-content-stage-local"
              class="overflow-hidden rounded-panel border border-dashed border-border bg-panel-field hover:border-accent has-[:disabled]:cursor-not-allowed has-[:disabled]:opacity-50"
              :class="localDropActive ? 'border-accent bg-hover' : ''"
              @dragenter.prevent="localDropActive = true"
              @dragover.prevent="localDropActive = true"
              @dragleave.prevent="localDropActive = false"
              @drop="handleLocalDrop"
            >
              <div class="flex h-52 items-center justify-center p-2">
                <template v-if="localPreviewUrl">
                  <EmbeddedDisplayContentPreview
                    :src="localPreviewUrl"
                    :alt="localContentLabel"
                    :placement="imagePlacement"
                    :background-color="frameBackgroundColor"
                    :target-width="selectedProfile?.resolution.width || 1"
                    :target-height="selectedProfile?.resolution.height || 1"
                    :round="selectedProfile?.visibleArea?.shape === 'round'"
                    class="max-h-48 w-48 max-w-[76%]"
                  />
                </template>
                <div v-else class="flex flex-col items-center justify-center gap-1 text-muted">
                  <icon-lucide-upload-cloud class="size-5 text-accent" />
                  <span class="text-[10px]">拖入图片、GIF 或 PNG 序列</span>
                </div>
              </div>
              <div class="grid h-24 min-w-0 grid-rows-3 border-t border-border px-3">
                <p class="flex min-w-0 items-center truncate text-[11px] font-medium text-surface">
                  {{ localContentPrimary }}
                </p>
                <p class="flex min-w-0 items-center truncate text-[9px] text-muted">
                  {{ localContentSecondary }}
                </p>
                <div class="flex min-w-0 items-center gap-1.5">
                  <button
                    type="button"
                    class="flex h-7 min-w-0 items-center gap-1.5 rounded-panel border border-border bg-canvas px-2 text-[9px] text-surface hover:bg-hover"
                    :disabled="uploadTaskRunning"
                    @click="localFileInput?.click()"
                  >
                    <icon-lucide-folder-open class="size-3.5 shrink-0" />
                    <span class="truncate">选择文件</span>
                  </button>
                  <button
                    v-if="localContentFiles.length > 1"
                    type="button"
                    class="flex h-7 items-center gap-1 rounded-panel border border-border bg-canvas px-2 text-[9px] text-surface hover:bg-hover"
                    @click.prevent="localPreviewPlaying = !localPreviewPlaying"
                  >
                    <icon-lucide-pause v-if="localPreviewPlaying" class="size-3" />
                    <icon-lucide-play v-else class="size-3" />
                    {{ localPreviewPlaying ? '暂停' : '播放' }}
                  </button>
                </div>
              </div>
              <input
                ref="localFileInput"
                class="sr-only"
                type="file"
                accept="image/gif,image/png,image/jpeg,image/webp,image/bmp"
                multiple
                :disabled="uploadTaskRunning"
                @change="handleLocalContentChange"
              />
            </div>
          </template>
        </div>
      </PanelSection>

      <div v-if="transportMode === 'wifi-live' && embeddedDisplayAdvancedDebugMode">
        <WifiLiveMirrorPanel
          :key="wifiLiveFirmwareRevision"
          :profile="selectedProfile"
          :bake-state="bakeState"
          :bake-frame-by-id="bakeFrameById"
          :background-color="frameBackgroundColor"
          :placement="imagePlacement"
          @busy-change="liveMirrorBusy = $event"
        />
      </div>

      <PanelSection v-if="embeddedDisplayAdvancedDebugMode" label="状态日志" :default-open="false">
        <pre
          class="min-h-16 max-h-48 overflow-auto rounded-panel border border-border bg-canvas p-2 text-[10px] leading-relaxed text-muted"
          >{{ buildLog.length ? buildLog.join('\n') : buildMessage }}</pre
        >
      </PanelSection>
    </div>

    <div
      v-if="uploadTaskRunning || uploadTaskStatus !== 'idle'"
      class="shrink-0 border-t border-border bg-panel px-panel py-2"
    >
      <div class="flex items-center gap-2">
        <div class="min-w-0 flex-1">
          <p class="truncate text-[10px] text-surface">{{ uploadTaskMessage }}</p>
          <div
            v-if="uploadTaskRunning"
            class="mt-1 h-1 overflow-hidden rounded-full bg-panel-field"
          >
            <div
              class="h-full bg-accent transition-[width]"
              :style="{ width: `${Math.max(3, uploadTaskProgress)}%` }"
            />
          </div>
        </div>
        <button
          v-if="uploadTaskRunning"
          type="button"
          class="flex h-7 shrink-0 items-center gap-1 rounded-panel border border-border bg-canvas px-2 text-[10px] text-surface hover:bg-hover"
          @click="cancelUploadTask"
        >
          <icon-lucide-x class="size-3" />
          取消
        </button>
      </div>
    </div>
  </div>
</template>
