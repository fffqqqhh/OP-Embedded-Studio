import { bakeEmbeddedFrameByIdFromSource } from '@/app/editor/embedded-display-bake'
import {
  cancelUsbFrameDeployment,
  executeUsbFrameDeployment,
  getActiveEmbeddedDisplayProfile,
  getActiveEmbeddedImageSettings,
  getUsbFrameDeploymentPlan,
  hasRememberedUsbFirmware,
  prepareUsbFrameDeployment,
  setActiveEmbeddedImageSettings,
  updateUsbFrameDeploymentAdaptation,
  type EmbeddedImagePlacement,
  type EmbeddedDesignSource,
  type UsbFrameDeploymentPlan
} from '@/features/embedded-display'

import { rememberUsbDeployment, rememberUsbFirmware, resolveDesignHandoffFrame } from './memory'

const planSources = new Map<string, EmbeddedDesignSource>()

function prunePlanSources(): void {
  for (const planId of planSources.keys()) {
    const status = getUsbFrameDeploymentPlan(planId)?.status
    if (!status || ['success', 'cancelled', 'superseded', 'stale'].includes(status)) {
      planSources.delete(planId)
    }
  }
}

export async function prepareUsbFrameDeploymentFromSource(
  source: EmbeddedDesignSource,
  backgroundColor?: string,
  placement?: EmbeddedImagePlacement
): Promise<UsbFrameDeploymentPlan> {
  const frame = resolveDesignHandoffFrame(source)
  if (!frame.available) {
    throw new Error(frame.reason || '请先选择一个 Frame、图片或 Frame 内的元素')
  }
  const file = await bakeEmbeddedFrameByIdFromSource(source, frame.id)
  if (!file) throw new Error('无法渲染当前画面，请重新选择后再试')
  const profile = getActiveEmbeddedDisplayProfile()
  const settings = getActiveEmbeddedImageSettings()
  const plan = await prepareUsbFrameDeployment({
    profile,
    frame: {
      id: frame.id,
      name: frame.name,
      revision: frame.revision,
      width: frame.width,
      height: frame.height
    },
    file,
    backgroundColor: backgroundColor ?? settings.backgroundColor,
    placement: placement ?? settings.placement,
    firstDeployment: !hasRememberedUsbFirmware(profile.id),
    scopeKey: source
  })
  setActiveEmbeddedImageSettings({
    placement: plan.placement,
    backgroundColor: plan.backgroundColor
  })
  prunePlanSources()
  planSources.set(plan.id, source)
  return plan
}

export function cancelUsbFrameDeploymentFromChat(planId: string): void {
  cancelUsbFrameDeployment(planId)
  prunePlanSources()
}

export async function updateUsbFrameDeploymentAdaptationFromChat(
  planId: string,
  placement: EmbeddedImagePlacement,
  backgroundColor?: string
): Promise<boolean> {
  const updated = await updateUsbFrameDeploymentAdaptation(planId, {
    placement,
    backgroundColor
  })
  if (!updated) return false
  const plan = getUsbFrameDeploymentPlan(planId)
  if (plan) {
    setActiveEmbeddedImageSettings({
      placement: plan.placement,
      backgroundColor: plan.backgroundColor
    })
  }
  return true
}

export async function executeUsbFrameDeploymentFromChat(planId: string): Promise<boolean> {
  const source = planSources.get(planId)
  const plan = getUsbFrameDeploymentPlan(planId)
  if (!source || !plan) return false
  const result = await executeUsbFrameDeployment(planId, {
    isSnapshotCurrent: () => {
      return (
        Boolean(source.getSource(plan.frame.id)) &&
        source.getRevision() === plan.frame.revision &&
        getActiveEmbeddedDisplayProfile().id === plan.profileId
      )
    },
    onFirmwareVerified: rememberUsbFirmware,
    onSuccess: rememberUsbDeployment
  })
  prunePlanSources()
  return result
}
