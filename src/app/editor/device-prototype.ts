import type { EditorStore } from '@/app/editor/session'
import type {
  DevicePrototypeFrameCandidate,
  DevicePrototypeFrameRender,
  DevicePrototypeInteraction
} from '@/features/device-prototype'
import { resolveDevicePrototypeTransitions } from '@/features/device-prototype'
import type {
  EmbeddedDesignSource,
  EmbeddedDesignSourceItem,
  EmbeddedAnimatedPrototypeBakeResult,
  EmbeddedPrototypeBakeResult
} from '@/features/embedded-display'

import { createEmbeddedDesignSource } from './embedded-design-source'
import { getEmbeddedFrameBakeStateFromSource } from './embedded-display-bake'

function candidatesFromItems(items: EmbeddedDesignSourceItem[]): DevicePrototypeFrameCandidate[] {
  const nameCounts = new Map<string, number>()
  for (const item of items) {
    const name = item.name.trim() || '未命名画面'
    nameCounts.set(name, (nameCounts.get(name) ?? 0) + 1)
  }
  const nameIndexes = new Map<string, number>()
  return items.map((item) => {
    const baseName = item.name.trim() || '未命名画面'
    const index = (nameIndexes.get(baseName) ?? 0) + 1
    nameIndexes.set(baseName, index)
    return {
      available: true,
      id: item.id,
      sourceKind: item.sourceKind,
      name: (nameCounts.get(baseName) ?? 0) > 1 ? `${baseName} (${index})` : baseName,
      width: item.width,
      height: item.height
    }
  })
}

export function getDevicePrototypeFrameCandidate(
  store: EditorStore
): DevicePrototypeFrameCandidate {
  return getDevicePrototypeFrameCandidateFromSource(createEmbeddedDesignSource(store))
}

export function getDevicePrototypeFrameCandidateFromSource(
  source: EmbeddedDesignSource
): DevicePrototypeFrameCandidate {
  const state = getEmbeddedFrameBakeStateFromSource(source)
  return {
    available: state.available,
    id: state.id,
    sourceKind: state.sourceKind,
    name: state.name,
    width: state.width,
    height: state.height,
    reason: state.reason
  }
}

export function getSelectedDevicePrototypeFrameCandidates(
  store: EditorStore
): DevicePrototypeFrameCandidate[] {
  return getSelectedDevicePrototypeFrameCandidatesFromSource(createEmbeddedDesignSource(store))
}

export function getSelectedDevicePrototypeFrameCandidatesFromSource(
  source: EmbeddedDesignSource
): DevicePrototypeFrameCandidate[] {
  return candidatesFromItems(source.getSelectedSources())
}

export function getDevicePrototypeFrameCandidates(
  store: EditorStore
): DevicePrototypeFrameCandidate[] {
  return getDevicePrototypeFrameCandidatesFromSource(createEmbeddedDesignSource(store))
}

export function getDevicePrototypeFrameCandidatesFromSource(
  source: EmbeddedDesignSource
): DevicePrototypeFrameCandidate[] {
  return candidatesFromItems(source.getPageSources())
}

export function createDevicePrototypeFrameRenderer(store: EditorStore): DevicePrototypeFrameRender {
  return createDevicePrototypeFrameRendererFromSource(createEmbeddedDesignSource(store))
}

export function createDevicePrototypeFrameRendererFromSource(
  source: EmbeddedDesignSource
): DevicePrototypeFrameRender {
  return async (frameId) => {
    if (!source.getSource(frameId)) throw new Error('交互引用的画面已不存在')
    const data = await source.renderSourcePng(frameId)
    return new Blob([Uint8Array.from(data).buffer], { type: 'image/png' })
  }
}

export async function bakeDevicePrototype(
  store: EditorStore,
  interaction: DevicePrototypeInteraction
): Promise<EmbeddedPrototypeBakeResult> {
  return bakeDevicePrototypeFromSource(createEmbeddedDesignSource(store), interaction)
}

export async function bakeDevicePrototypeFromSource(
  source: EmbeddedDesignSource,
  interaction: DevicePrototypeInteraction
): Promise<EmbeddedPrototypeBakeResult> {
  const states = []
  for (const state of interaction.states) {
    if (!source.getSource(state.frameId)) {
      throw new Error(`交互引用的画面已不存在：${state.name}`)
    }
    const data = await source.renderSourcePng(state.frameId)
    states.push({
      id: state.id,
      name: state.name,
      file: new File([Uint8Array.from(data).buffer], `${state.name || 'state'}.png`, {
        type: 'image/png'
      })
    })
  }

  return {
    id: interaction.id,
    name: interaction.name,
    mode: interaction.mode,
    intervalMs: interaction.slideshow.intervalMs,
    initialStateId: interaction.initialStateId,
    states,
    transitions: resolveDevicePrototypeTransitions(interaction).map((transition) => ({
      ...transition
    }))
  }
}

export function bakeDevicePrototypeAnimation(
  interaction: DevicePrototypeInteraction
): EmbeddedAnimatedPrototypeBakeResult {
  if (!interaction.states.length) throw new Error('动画交互至少需要一个状态')
  const states = interaction.states.map((state) => {
    if (!state.animation?.files.length) {
      throw new Error(`状态“${state.name}”不是 PNG 动画状态，不能与动画交互固件混用`)
    }
    return {
      id: state.id,
      name: state.name,
      frameDelayMs: state.animation.frameDelayMs,
      loop: state.animation.loop,
      files: [...state.animation.files]
    }
  })
  return {
    id: interaction.id,
    name: interaction.name,
    initialStateId: interaction.initialStateId,
    states,
    transitions: resolveDevicePrototypeTransitions(interaction).map((transition) => ({
      ...transition
    }))
  }
}
