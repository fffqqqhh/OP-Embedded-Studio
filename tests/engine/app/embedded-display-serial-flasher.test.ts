import { describe, expect, test } from 'bun:test'

import {
  PREBUILT_IMAGE_FLASH_PARAMS,
  resetDevice
} from '@/features/embedded-display/adapters/serial-flasher'

describe('embedded display serial flasher image parameters', () => {
  test('preserves the flash parameters and digest embedded in prebuilt images', () => {
    expect(PREBUILT_IMAGE_FLASH_PARAMS).toEqual({
      flashMode: 'keep',
      flashFreq: 'keep',
      flashSize: 'keep'
    })
  })
})

describe('embedded display serial flasher reset', () => {
  test('uses the USB hard-reset sequence after flashing', async () => {
    const events: string[] = []
    const transport = {
      setDTR: async (state: boolean) => {
        events.push(`dtr:${state}`)
      },
      setRTS: async (state: boolean) => {
        events.push(`rts:${state}`)
      }
    }
    const loader = {
      after: async (mode: string, usingUsbOtg?: boolean) => {
        events.push(`after:${mode}:${usingUsbOtg}`)
        await transport.setRTS(false)
      }
    }

    await resetDevice(transport, loader, async (milliseconds) => {
      events.push(`wait:${milliseconds}`)
    })

    expect(events).toEqual([
      'dtr:false',
      'rts:true',
      'after:hard_reset:true',
      'rts:false',
      'dtr:false',
      'wait:1500'
    ])
  })
})
