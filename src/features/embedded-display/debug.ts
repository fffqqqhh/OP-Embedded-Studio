import { useLocalStorage } from '@vueuse/core'

export const embeddedDisplayAdvancedDebugMode = useLocalStorage(
  'op-embedded-display-advanced-debug',
  false
)
