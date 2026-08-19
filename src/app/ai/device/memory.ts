import { readCacheJson, writeCacheJson } from '@/app/cache'
import { getEmbeddedFrameBakeStateFromSource } from '@/app/editor/embedded-display-bake'
import {
  type EmbeddedDesignSource,
  rememberUsbFirmwareForPort,
  type EmbeddedFrameBakeState,
  type UsbFrameDeploymentPlan
} from '@/features/embedded-display'

interface RecentAIDesign {
  frameId: string
  frameName: string
  revision: number
  observation: string
  intent: string
  changes: string[]
  updatedAt: number
}

export interface DesignHandoffMemory {
  documentName: string
  revision: number
  frame?: {
    id: string
    name: string
    width: number
    height: number
    layerCount: number
    textSamples: string[]
    source: 'ai-assisted' | 'user-design'
    changedAfterAISummary: boolean
  }
  recentAI?: Omit<RecentAIDesign, 'frameId' | 'frameName' | 'revision'>
}

interface UsbDeploymentMemoryRecord {
  profileId: string
  profileName: string
  protocol: 'OPUSB/1'
  width: number
  height: number
  firmwareVerifiedAt: number
  lastFrameName?: string
  lastFrameRevision?: number
  lastDeployedAt?: number
}

type UsbDeploymentMemory = Partial<Record<string, UsbDeploymentMemoryRecord>>

const USB_DEPLOYMENT_MEMORY_KEY = 'embedded-display/ai-usb-frame-deployments'
const recentDesigns = new WeakMap<EmbeddedDesignSource, RecentAIDesign>()
const latestUsbDeployments = new Map<string, UsbDeploymentMemoryRecord>()

export function recordDesignHandoff(
  source: EmbeddedDesignSource,
  input: Omit<RecentAIDesign, 'revision' | 'updatedAt'>
): void {
  recentDesigns.set(source, {
    ...input,
    revision: source.getRevision(),
    updatedAt: Date.now()
  })
}

export function resolveDesignHandoffFrame(source: EmbeddedDesignSource): EmbeddedFrameBakeState {
  const selected = getEmbeddedFrameBakeStateFromSource(source)
  if (selected.available) return selected

  const revision = source.getRevision()
  const recent = recentDesigns.get(source)
  const recentFrame = recent ? source.getSource(recent.frameId) : null
  if (recentFrame?.sourceKind === 'frame') {
    return {
      id: recentFrame.id,
      revision,
      available: true,
      sourceKind: 'frame',
      name: recentFrame.name,
      width: recentFrame.width,
      height: recentFrame.height
    }
  }

  const topLevelFrames = source.getPageSources()
  if (topLevelFrames.length === 1) {
    const frame = topLevelFrames[0]
    return {
      id: frame.id,
      revision,
      available: true,
      sourceKind: frame.sourceKind,
      name: frame.name,
      width: frame.width,
      height: frame.height
    }
  }

  return selected
}

export function getDesignHandoffMemory(source: EmbeddedDesignSource): DesignHandoffMemory {
  const bakeState = resolveDesignHandoffFrame(source)
  const recent = recentDesigns.get(source)
  const memory: DesignHandoffMemory = {
    documentName: source.getDocumentName(),
    revision: source.getRevision()
  }
  if (!bakeState.available) return memory

  const summary = source.getSourceSummary(bakeState.id)
  const matchesRecentAI = recent?.frameId === bakeState.id
  memory.frame = {
    id: bakeState.id,
    name: bakeState.name,
    width: bakeState.width,
    height: bakeState.height,
    layerCount: summary?.layerCount ?? 1,
    textSamples: summary?.textSamples ?? [],
    source: matchesRecentAI ? 'ai-assisted' : 'user-design',
    changedAfterAISummary: matchesRecentAI && recent.revision !== bakeState.revision
  }
  if (matchesRecentAI) {
    memory.recentAI = {
      observation: recent.observation,
      intent: recent.intent,
      changes: recent.changes,
      updatedAt: recent.updatedAt
    }
  }
  return memory
}

async function readUsbDeploymentMemory(): Promise<UsbDeploymentMemory> {
  return (await readCacheJson<UsbDeploymentMemory>(USB_DEPLOYMENT_MEMORY_KEY)) ?? {}
}

export async function rememberUsbFirmware(plan: UsbFrameDeploymentPlan): Promise<void> {
  rememberUsbFirmwareForPort(plan.profileId)
  const memory = await readUsbDeploymentMemory()
  const previous = memory[plan.profileId]
  memory[plan.profileId] = {
    ...previous,
    profileId: plan.profileId,
    profileName: plan.profileName,
    protocol: 'OPUSB/1',
    width: plan.resolution.width,
    height: plan.resolution.height,
    firmwareVerifiedAt: Date.now()
  }
  const updated = memory[plan.profileId]
  if (updated) latestUsbDeployments.set(plan.profileId, updated)
  await writeCacheJson(USB_DEPLOYMENT_MEMORY_KEY, memory)
}

export async function rememberUsbDeployment(plan: UsbFrameDeploymentPlan): Promise<void> {
  const memory = await readUsbDeploymentMemory()
  const previous = memory[plan.profileId]
  memory[plan.profileId] = {
    ...previous,
    profileId: plan.profileId,
    profileName: plan.profileName,
    protocol: 'OPUSB/1',
    width: plan.resolution.width,
    height: plan.resolution.height,
    firmwareVerifiedAt: previous?.firmwareVerifiedAt ?? Date.now(),
    lastFrameName: plan.frame.name,
    lastFrameRevision: plan.frame.revision,
    lastDeployedAt: Date.now()
  }
  const updated = memory[plan.profileId]
  if (updated) latestUsbDeployments.set(plan.profileId, updated)
  await writeCacheJson(USB_DEPLOYMENT_MEMORY_KEY, memory)
}

export function getLatestUsbDeploymentMemory(
  profileId: string
): UsbDeploymentMemoryRecord | undefined {
  return latestUsbDeployments.get(profileId)
}
