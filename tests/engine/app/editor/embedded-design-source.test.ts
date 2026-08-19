import { describe, expect, test } from 'bun:test'

import { SceneGraph } from '@open-pencil/scene-graph'

import { createEmbeddedDesignSource } from '@/app/editor/embedded-design-source'
import { createEditorStore } from '@/app/editor/session'

describe('embedded design source adapter', () => {
  test('projects selected and page visual sources without exposing scene nodes', () => {
    const graph = new SceneGraph()
    const pageId = graph.getPages()[0].id
    const frame = graph.createNode('FRAME', pageId, {
      name: 'Home screen',
      width: 466,
      height: 466
    })
    const image = graph.createNode('RECTANGLE', pageId, {
      name: 'Background image',
      width: 320,
      height: 240,
      fills: [
        {
          type: 'IMAGE',
          imageHash: 'image-hash',
          imageScaleMode: 'FILL',
          color: { r: 0, g: 0, b: 0, a: 0 },
          opacity: 1,
          visible: true
        }
      ]
    })
    const store = createEditorStore(graph)
    store.select([frame.id])

    const source = createEmbeddedDesignSource(store)

    expect(source.getSelectedSources()).toEqual([
      {
        id: frame.id,
        name: 'Home screen',
        sourceKind: 'frame',
        width: 466,
        height: 466
      }
    ])
    expect(source.getPageSources()).toEqual([
      expect.objectContaining({ id: frame.id, sourceKind: 'frame' }),
      expect.objectContaining({ id: image.id, sourceKind: 'image' })
    ])
    expect(source.getSource(frame.id)).toEqual(
      expect.objectContaining({ id: frame.id, sourceKind: 'frame' })
    )
    expect(source.getSource('missing')).toBeNull()
    expect(source.getRevision()).toBe(store.state.sceneVersion)
  })

  test('resolves a selected child through the existing embedded source rules', () => {
    const graph = new SceneGraph()
    const pageId = graph.getPages()[0].id
    const frame = graph.createNode('FRAME', pageId, { width: 240, height: 240 })
    const child = graph.createNode('RECTANGLE', frame.id, { width: 20, height: 20 })
    const store = createEditorStore(graph)
    store.select([child.id])

    expect(createEmbeddedDesignSource(store).getSelectedSources()).toEqual([
      expect.objectContaining({ id: frame.id, sourceKind: 'frame' })
    ])
  })
})
