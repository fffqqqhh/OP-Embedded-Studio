import type { SceneNode } from '@open-pencil/scene-graph'

import type { EditorStore } from '@/app/editor/session'
import type {
  EmbeddedDesignSource,
  EmbeddedDesignSourceItem
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

export function createEmbeddedDesignSource(store: EditorStore): EmbeddedDesignSource {
  return {
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

    renderSourcePng: (id) => renderEmbeddedVisualPng(store, id)
  }
}
