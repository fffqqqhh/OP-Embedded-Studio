import type { SceneNode } from '@open-pencil/scene-graph'

import type { EditorStore } from '@/app/editor/session'
import type {
  EmbeddedDesignSource,
  EmbeddedDesignSourceItem
} from '@/features/embedded-display/model/design-source'

import { getSelectedEmbeddedVisualSources, isEmbeddedVisualSource } from './embedded-display-bake'
import { renderEmbeddedVisualPng } from './embedded-frame-render'

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
