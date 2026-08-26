import { describe, expect, test } from 'bun:test'

import {
  DEFAULT_EMBEDDED_DISPLAY_PROFILE_ID,
  bundledDisplayProfiles,
  bundledFirmwareManifestUrl
} from '../../../src/features/embedded-display/runtime/catalog'

describe('embedded display runtime catalog', () => {
  test('loads device profiles without the local build service', () => {
    const profiles = bundledDisplayProfiles()
    expect(profiles.map((profile) => profile.id)).toEqual([
      'st7789_qs130tab1005a',
      'st7735s_lb090r_if03',
      'gc9d01n_gvh099wq010b_a0',
      'co5300_waveshare_amoled_1_75c',
      'co5300_m5stack_stopwatch',
      'ili9342_m5stack_cores3',
      'gc9a01_xf_gf110648',
      'st77916_xf_gf132a159'
    ])
    expect(profiles.some((profile) => profile.id === DEFAULT_EMBEDDED_DISPLAY_PROFILE_ID)).toBe(
      true
    )
  })

  test('exposes every verified imported display in all requested firmware modes', () => {
    const importedProfileIds = [
      'st7789_qs130tab1005a',
      'st7735s_lb090r_if03',
      'gc9d01n_gvh099wq010b_a0',
      'gc9a01_xf_gf110648',
      'st77916_xf_gf132a159'
    ]
    const modes = ['usb-frame', 'wifi-frame', 'wifi-live', 'ble-frame'] as const

    for (const profileId of importedProfileIds) {
      for (const mode of modes) {
        expect(bundledFirmwareManifestUrl(profileId, mode)).not.toBeNull()
      }
    }
  })

  test('exposes bundled wireless firmware independently by mode', () => {
    const profileId = 'co5300_waveshare_amoled_1_75c'
    expect(bundledFirmwareManifestUrl(profileId, 'usb-frame')).toContain(
      '/embedded-display/firmware/usb-frame/'
    )
    expect(bundledFirmwareManifestUrl(profileId, 'wifi-frame')).toContain(
      '/embedded-display/firmware/wifi-frame/'
    )
    expect(bundledFirmwareManifestUrl(profileId, 'wifi-live')).toContain(
      '/embedded-display/firmware/wifi-live/'
    )
    expect(bundledFirmwareManifestUrl(profileId, 'ble-frame')).toContain(
      '/embedded-display/firmware/ble-frame/'
    )
    expect(bundledFirmwareManifestUrl(profileId, 'usb-prototype')).toBeNull()
  })
})
