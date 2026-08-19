import type { EditorStore } from '@/app/editor/session'
import type {
  EmbeddedDesignSource,
  EmbeddedDesignSourceItem,
  EmbeddedFrameBakeState
} from '@/features/embedded-display'

import { createEmbeddedDesignSource } from './embedded-design-source'

export {
  getSelectedEmbeddedVisualSources,
  isEmbeddedVisualSource,
  resolveEmbeddedVisualSelection
} from './embedded-visual-source'

export function getEmbeddedFrameBakeState(store: EditorStore): EmbeddedFrameBakeState {
  return getEmbeddedFrameBakeStateFromSource(createEmbeddedDesignSource(store))
}

function unavailableFrameBakeState(source: EmbeddedDesignSource, reason: string) {
  return {
    id: '',
    revision: source.getRevision(),
    available: false,
    sourceKind: 'frame' as const,
    name: '',
    width: 0,
    height: 0,
    reason
  }
}

export function getEmbeddedFrameBakeStateFromSource(
  source: EmbeddedDesignSource
): EmbeddedFrameBakeState {
  const selected = source.getSelectedSources()
  if (selected.length === 0) {
    return unavailableFrameBakeState(
      source,
      source.getSelectionError?.() ?? '请选择一个 Frame、图片或 Frame 内的元素'
    )
  }
  if (selected.length > 1) {
    return unavailableFrameBakeState(
      source,
      `已选中 ${selected.length} 个画面，请使用下方的交互烧录`
    )
  }

  const item = selected[0]
  if (!item) {
    return unavailableFrameBakeState(source, '当前选择不是可烧录的 Frame 或图片')
  }
  return {
    id: item.id,
    revision: source.getRevision(),
    available: true,
    sourceKind: item.sourceKind,
    name: item.name,
    width: item.width,
    height: item.height
  }
}

function sourceFileName(item: EmbeddedDesignSourceItem): string {
  return item.name.trim().replace(/[^a-zA-Z0-9\u4e00-\u9fff_-]+/g, '_') || 'frame'
}

export async function bakeEmbeddedFrameByIdFromSource(
  source: EmbeddedDesignSource,
  frameId: string
): Promise<File | null> {
  const item = source.getSource(frameId)
  if (!item) return null
  const data = await source.renderSourcePng(item.id)
  return new File([new Uint8Array(data)], `${sourceFileName(item)}.png`, { type: 'image/png' })
}

export async function bakeEmbeddedFrameFromSource(
  source: EmbeddedDesignSource
): Promise<File | null> {
  const state = getEmbeddedFrameBakeStateFromSource(source)
  if (!state.available) return null
  return bakeEmbeddedFrameByIdFromSource(source, state.id)
}

export async function bakeEmbeddedFrameById(store: EditorStore, frameId: string) {
  return bakeEmbeddedFrameByIdFromSource(createEmbeddedDesignSource(store), frameId)
}

export async function bakeEmbeddedFrame(store: EditorStore): Promise<File | null> {
  return bakeEmbeddedFrameFromSource(createEmbeddedDesignSource(store))
}
