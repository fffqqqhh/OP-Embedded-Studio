import { useLocalStorage } from '@vueuse/core'
import { computed } from 'vue'

type LayerTreeDisplayOrder = 'document' | 'front-first'

export const layerTreeDisplayOrder = useLocalStorage<LayerTreeDisplayOrder>(
  'op-layer-tree-display-order',
  'document'
)

export const showTopLayersFirst = computed({
  get: () => layerTreeDisplayOrder.value === 'front-first',
  set: (enabled: boolean) => {
    layerTreeDisplayOrder.value = enabled ? 'front-first' : 'document'
  }
})
