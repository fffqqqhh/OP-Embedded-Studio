import { describe, expect, test } from 'bun:test'

import {
  executeUsbFrameDeploymentFromChat,
  prepareUsbFrameDeploymentFromSource
} from '@/app/ai/device/deployment'
import {
  getDesignHandoffMemory,
  recordDesignHandoff,
  resolveDesignHandoffFrame
} from '@/app/ai/device/memory'
import {
  confirmDevicePrototypeProposalFromChat,
  executeDevicePrototypeDeploymentFromChat,
  getDevicePrototypeDeploymentPlan,
  prepareDevicePrototypeProposal,
  renderDevicePrototypeProposalFrame
} from '@/app/ai/device/prototype'
import { getDevicePrototypeFrameCandidatesFromSource } from '@/app/editor/device-prototype'
import {
  bakeEmbeddedFrameByIdFromSource,
  getEmbeddedFrameBakeStateFromSource
} from '@/app/editor/embedded-display-bake'
import { getUsbFrameDeploymentPlan } from '@/features/embedded-display'

import { FakeEmbeddedDesignSource } from '#tests/helpers/embedded-design-source'

const HOME = {
  id: 'home',
  name: 'Home',
  sourceKind: 'frame' as const,
  width: 240,
  height: 240,
  png: new Uint8Array([1, 2, 3]),
  summary: { layerCount: 3, textSamples: ['23 C', 'Comfort'] }
}

const DETAIL = {
  id: 'detail',
  name: 'Detail',
  sourceKind: 'image' as const,
  width: 320,
  height: 180,
  png: new Uint8Array([4, 5, 6]),
  summary: { layerCount: 0, textSamples: [] }
}

async function withImageConversionMocks<T>(run: () => Promise<T>): Promise<T> {
  const descriptors = {
    createImageBitmap: Object.getOwnPropertyDescriptor(globalThis, 'createImageBitmap'),
    document: Object.getOwnPropertyDescriptor(globalThis, 'document')
  }
  Object.defineProperty(globalThis, 'createImageBitmap', {
    configurable: true,
    value: async () => ({ width: 1, height: 1, close: () => undefined })
  })
  Object.defineProperty(globalThis, 'document', {
    configurable: true,
    value: {
      createElement: () => {
        const canvas = {
          width: 0,
          height: 0,
          getContext: () => ({
            fillStyle: '',
            imageSmoothingEnabled: true,
            clearRect: () => undefined,
            fillRect: () => undefined,
            drawImage: () => undefined,
            getImageData: () => ({
              data: new Uint8ClampedArray(canvas.width * canvas.height * 4)
            })
          }),
          toBlob: (callback: (blob: Blob | null) => void) => callback(new Blob([new Uint8Array(4)]))
        }
        return canvas
      }
    }
  })

  try {
    return await run()
  } finally {
    for (const [key, descriptor] of Object.entries(descriptors)) {
      if (descriptor) Object.defineProperty(globalThis, key, descriptor)
      else Reflect.deleteProperty(globalThis, key)
    }
  }
}

describe('embedded design source product boundary', () => {
  test('resolves selection and single-page fallback without an editor store', () => {
    const selected = new FakeEmbeddedDesignSource([HOME, DETAIL])
    selected.select([DETAIL.id])

    expect(getEmbeddedFrameBakeStateFromSource(selected)).toMatchObject({
      available: true,
      id: DETAIL.id,
      sourceKind: 'image'
    })
    expect(getDevicePrototypeFrameCandidatesFromSource(selected).map(({ id }) => id)).toEqual([
      HOME.id,
      DETAIL.id
    ])

    const fallback = new FakeEmbeddedDesignSource([HOME])
    expect(resolveDesignHandoffFrame(fallback)).toMatchObject({
      available: true,
      id: HOME.id
    })
    fallback.setSelectionError('当前选择不是可烧录的 Frame 或图片')
    expect(getEmbeddedFrameBakeStateFromSource(fallback).reason).toBe(
      '当前选择不是可烧录的 Frame 或图片'
    )
  })

  test('keeps AI handoff memory and revision checks on the source contract', () => {
    const source = new FakeEmbeddedDesignSource([HOME, DETAIL], 'Boundary document')
    recordDesignHandoff(source, {
      frameId: HOME.id,
      frameName: HOME.name,
      observation: 'Created the home screen',
      intent: 'Show the current environment',
      changes: ['Added the primary reading']
    })

    expect(getDesignHandoffMemory(source)).toMatchObject({
      documentName: 'Boundary document',
      frame: {
        id: HOME.id,
        layerCount: 3,
        textSamples: ['23 C', 'Comfort'],
        source: 'ai-assisted',
        changedAfterAISummary: false
      }
    })

    source.advanceRevision()
    expect(getDesignHandoffMemory(source).frame?.changedAfterAISummary).toBe(true)
  })

  test('renders and proposes interactions using fake source bytes', async () => {
    const source = new FakeEmbeddedDesignSource([HOME, DETAIL])
    const file = await bakeEmbeddedFrameByIdFromSource(source, HOME.id)
    expect(file && new Uint8Array(await file.arrayBuffer())).toEqual(HOME.png)

    const proposal = prepareDevicePrototypeProposal(source, {
      intent: 'Tap to open details',
      name: 'Boundary navigation',
      frameIds: [HOME.id, DETAIL.id],
      initialFrameId: HOME.id,
      transitions: [{ fromFrameId: HOME.id, event: 'screen_click', toFrameId: DETAIL.id }]
    })
    expect(proposal.definition.states.map(({ id }) => id)).toEqual([HOME.id, DETAIL.id])
    const preview = await renderDevicePrototypeProposalFrame(proposal.id, DETAIL.id)
    expect(preview && new Uint8Array(await preview.arrayBuffer())).toEqual(DETAIL.png)
  })

  test('retains the originating source when validating a frame deployment', async () => {
    await withImageConversionMocks(async () => {
      const source = new FakeEmbeddedDesignSource([HOME])
      source.select([HOME.id])
      const plan = await prepareUsbFrameDeploymentFromSource(source)

      source.removeSource(HOME.id)
      expect(await executeUsbFrameDeploymentFromChat(plan.id)).toBe(false)
      expect(getUsbFrameDeploymentPlan(plan.id)).toMatchObject({
        status: 'stale',
        frame: { id: HOME.id }
      })
    })
  })

  test('marks a confirmed prototype stale after the source revision changes', async () => {
    await withImageConversionMocks(async () => {
      const source = new FakeEmbeddedDesignSource([HOME, DETAIL])
      const proposal = prepareDevicePrototypeProposal(source, {
        intent: 'Tap to open details',
        name: 'Revision boundary',
        frameIds: [HOME.id, DETAIL.id],
        initialFrameId: HOME.id,
        transitions: [{ fromFrameId: HOME.id, event: 'screen_click', toFrameId: DETAIL.id }]
      })

      expect(await confirmDevicePrototypeProposalFromChat(proposal.id)).toBe(true)
      source.advanceRevision()
      expect(await executeDevicePrototypeDeploymentFromChat(proposal.id)).toBe(false)
      expect(getDevicePrototypeDeploymentPlan(proposal.id)?.status).toBe('stale')
    })
  })
})
