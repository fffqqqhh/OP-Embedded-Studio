import type { SceneNode } from '@open-pencil/scene-graph'

import type { EditorStore } from '@/app/editor/session'

export function isEmbeddedVisualSource(node: SceneNode | undefined): node is SceneNode {
  if (!node) return false
  if (node.type === 'FRAME') return true
  return node.fills.some((fill) => fill.visible && fill.type === 'IMAGE' && Boolean(fill.imageHash))
}

function findNearestFrame(store: EditorStore, nodeId: string): SceneNode | null {
  const visited = new Set<string>()
  let node = store.graph.getNode(nodeId)

  while (node && !visited.has(node.id)) {
    visited.add(node.id)
    if (node.type === 'CANVAS') return null
    if (node.type === 'FRAME' && node.id !== store.graph.rootId) return node
    if (!node.parentId) return null
    node = store.graph.getNode(node.parentId)
  }
  return null
}

function resolveEmbeddedVisualSource(store: EditorStore, nodeId: string): SceneNode | null {
  const frame = findNearestFrame(store, nodeId)
  if (frame) return frame
  const node = store.graph.getNode(nodeId)
  return isEmbeddedVisualSource(node) && node.type !== 'FRAME' ? node : null
}

export function getSelectedEmbeddedVisualSources(store: EditorStore): SceneNode[] {
  return [
    ...new Map(
      [...store.state.selectedIds]
        .map((id) => resolveEmbeddedVisualSource(store, id))
        .filter((source): source is SceneNode => source !== null)
        .map((source) => [source.id, source])
    ).values()
  ]
}

export interface EmbeddedVisualSelection {
  source: SceneNode | null
  reason?: string
}

export function resolveEmbeddedVisualSelection(store: EditorStore): EmbeddedVisualSelection {
  const selectedIds = [...store.state.selectedIds]
  if (selectedIds.length === 0) {
    return { source: null, reason: '请选择一个 Frame、图片或 Frame 内的元素' }
  }

  const sources = selectedIds.map((id) => resolveEmbeddedVisualSource(store, id))
  const resolvedSources = sources.filter((source): source is SceneNode => source !== null)
  if (resolvedSources.length !== selectedIds.length) {
    return { source: null, reason: '当前选择不是可烧录的 Frame 或图片' }
  }

  const uniqueSources = new Map(resolvedSources.map((source) => [source.id, source]))
  if (uniqueSources.size !== 1) {
    return {
      source: null,
      reason: `已选中 ${uniqueSources.size} 个画面，请使用下方的交互烧录`
    }
  }
  return { source: uniqueSources.values().next().value ?? null }
}
