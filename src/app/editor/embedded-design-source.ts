import type { SceneNode } from '@open-pencil/scene-graph'

import type { EditorStore } from '@/app/editor/session'
import type {
  EmbeddedDesignSource,
  EmbeddedDesignSourceItem,
  EmbeddedDesignSourceSummary
} from '@/features/embedded-display/model/design-source'

import { renderEmbeddedVisualPng } from './embedded-frame-render'
import {
  getSelectedEmbeddedVisualSources,
  isEmbeddedVisualSource,
  resolveEmbeddedVisualSelection
} from './embedded-visual-source'

function toSourceItem(node: SceneNode): EmbeddedDesignSourceItem {
  return {
    id: node.id,
    name: node.name,
    sourceKind: node.type === 'FRAME' ? 'frame' : 'image',
    width: node.width,
    height: node.height
  }
}

const designSources = new WeakMap<EditorStore, EmbeddedDesignSource>()

function sourceSummary(store: EditorStore, id: string): EmbeddedDesignSourceSummary | null {
  const node = store.graph.getNode(id)
  if (!isEmbeddedVisualSource(node) || node.id === store.graph.rootId) return null
  const flattened = store.graph.flattenTree(id)
  return {
    layerCount: flattened.length,
    textSamples: flattened
      .map(({ node: child }) =>
        child.type === 'TEXT' && 'characters' in child && typeof child.characters === 'string'
          ? child.characters.trim()
          : ''
      )
      .filter(Boolean)
      .slice(0, 8)
  }
}

export function createEmbeddedDesignSource(store: EditorStore): EmbeddedDesignSource {
  const existing = designSources.get(store)
  if (existing) return existing

  const source: EmbeddedDesignSource = {
    getDocumentName: () => store.state.documentName,

    getRevision: () => store.state.sceneVersion,

    getSelectedSources: () => getSelectedEmbeddedVisualSources(store).map(toSourceItem),

    getSelectionError: () => {
      const selection = resolveEmbeddedVisualSelection(store)
      return selection.source ? null : (selection.reason ?? null)
    },

    getPageSources: () =>
      store.graph
        .getChildren(store.state.currentPageId)
        .filter((node) => isEmbeddedVisualSource(node) && node.id !== store.graph.rootId)
        .map(toSourceItem),

    getSource: (id) => {
      const node = store.graph.getNode(id)
      return isEmbeddedVisualSource(node) && node.id !== store.graph.rootId
        ? toSourceItem(node)
        : null
    },

    getSourceSummary: (id) => sourceSummary(store, id),

    renderSourcePng: (id) => renderEmbeddedVisualPng(store, id)
  }
  designSources.set(store, source)
  return source
}
