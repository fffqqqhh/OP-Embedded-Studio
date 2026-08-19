import { describe, expect, test } from 'bun:test'

import { createPresetFrameName } from '@/app/editor/preset-frame-name'

describe('preset frame names', () => {
  test('uses a short device name for known profiles', () => {
    expect(createPresetFrameName('M5Stack StopWatch 466x466 圆形 AMOLED', [])).toBe('StopWatch 1')
    expect(createPresetFrameName('M5Stack CoreS3 320x240 ILI9342C', [])).toBe('CoreS3 1')
  })

  test('increments names without renaming existing frames', () => {
    expect(
      createPresetFrameName('M5Stack StopWatch 466x466 圆形 AMOLED', [
        'StopWatch 1',
        'Other Frame',
        'StopWatch 2'
      ])
    ).toBe('StopWatch 3')
  })

  test('compacts unknown profile names', () => {
    expect(createPresetFrameName('Custom Display 320x240 ST7789 方屏', [])).toBe('Custom Display 1')
  })
})
