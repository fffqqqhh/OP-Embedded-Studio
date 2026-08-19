import { getDevicePrototypeFrameCandidatesFromSource } from '@/app/editor/device-prototype'
import {
  getActiveEmbeddedDisplayProfile,
  type EmbeddedDesignSource
} from '@/features/embedded-display'

import { getDesignHandoffMemory, getLatestUsbDeploymentMemory } from './memory'
import DEVICE_SYSTEM_PROMPT from './system-prompt.md?raw'

export function createDeviceSystemPrompt(source: EmbeddedDesignSource): string {
  const profile = getActiveEmbeddedDisplayProfile()
  const memory = getDesignHandoffMemory(source)
  const frame = memory.frame
    ? {
        ...memory.frame,
        recentAI: memory.recentAI
      }
    : null
  const interactionFrames = getDevicePrototypeFrameCandidatesFromSource(source).map((candidate) => {
    const textSamples = source.getSourceSummary(candidate.id)?.textSamples.slice(0, 6) ?? []
    return {
      id: candidate.id,
      name: candidate.name,
      sourceKind: candidate.sourceKind,
      width: candidate.width,
      height: candidate.height,
      textSamples
    }
  })
  return `${DEVICE_SYSTEM_PROMPT}\n\n# Active device target\n\n${JSON.stringify(
    {
      device: {
        id: profile.id,
        name: profile.name,
        resolution: profile.resolution,
        visibleArea: profile.visibleArea?.shape ?? 'rectangular'
      },
      design: {
        documentName: memory.documentName,
        revision: memory.revision,
        frame,
        interactionFrames
      },
      latestDeployment: getLatestUsbDeploymentMemory(profile.id) ?? null
    },
    null,
    2
  )}`
}
