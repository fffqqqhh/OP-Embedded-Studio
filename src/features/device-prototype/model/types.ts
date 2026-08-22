export const DEVICE_PROTOTYPE_EVENTS = [
  { id: 'screen_click', label: '屏幕单击' },
  { id: 'screen_long_press', label: '屏幕长按' },
  { id: 'screen_double_click', label: '屏幕双击' },
  { id: 'screen_triple_click', label: '屏幕三击' },
  { id: 'boot_click', label: 'BOOT 单击' },
  { id: 'boot_long_press', label: 'BOOT 长按' },
  { id: 'stopwatch_button_a_click', label: 'StopWatch A 键' },
  { id: 'stopwatch_button_b_click', label: 'StopWatch B 键' }
] as const

export const DEVICE_PROTOTYPE_MAX_STATES = 10

export type DevicePrototypeEventId = (typeof DEVICE_PROTOTYPE_EVENTS)[number]['id']
export type DevicePrototypeMode = 'manual' | 'slideshow' | 'custom'
export type DevicePrototypePortDirection = 'top' | 'right' | 'bottom' | 'left'

const STOPWATCH_EVENT_IDS = new Set<DevicePrototypeEventId>([
  'stopwatch_button_a_click',
  'stopwatch_button_b_click'
])

export function devicePrototypeEventsForProfile(profileId: string) {
  return DEVICE_PROTOTYPE_EVENTS.filter(
    (event) => profileId === 'co5300_m5stack_stopwatch' || !STOPWATCH_EVENT_IDS.has(event.id)
  )
}

export interface DevicePrototypeManualSettings {
  nextEvent: DevicePrototypeEventId
  previousEvent: DevicePrototypeEventId
  loop: boolean
}

export interface DevicePrototypeSlideshowSettings {
  intervalMs: number
}

export interface DevicePrototypeFrameCandidate {
  available: boolean
  id: string
  sourceKind: 'frame' | 'image'
  name: string
  width: number
  height: number
  reason?: string
}

export interface DevicePrototypeState {
  id: string
  frameId: string
  name: string
  width: number
  height: number
  animation?: {
    files: File[]
    frameDelayMs: number
    loop: boolean
  }
}

export interface DevicePrototypeTransition {
  fromStateId: string
  event: DevicePrototypeEventId
  toStateId: string
  fromPort?: DevicePrototypePortDirection
  toPort?: DevicePrototypePortDirection
}

export interface DevicePrototypeDefinition {
  initialStateId: string
  states: DevicePrototypeState[]
  transitions: DevicePrototypeTransition[]
}

export interface DevicePrototypeInteraction extends DevicePrototypeDefinition {
  id: string
  name: string
  mode: DevicePrototypeMode
  manual: DevicePrototypeManualSettings
  slideshow: DevicePrototypeSlideshowSettings
}

export interface DevicePrototypePreviewProfile {
  id: string
  name: string
  resolution: { width: number; height: number }
  visibleArea?: { shape?: string }
}

export interface DevicePrototypeInteractionOption {
  id: string
  name: string
  contentKind: 'prototype' | 'animated-prototype'
  mode: DevicePrototypeMode
  stateCount: number
  initialStateName: string
  intervalMs: number
  width: number
  height: number
  valid: boolean
  reason?: string
}

export type DevicePrototypeFrameRender = (frameId: string) => Promise<Blob | null>
