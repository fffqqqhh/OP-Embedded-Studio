<script setup lang="ts">
const { status, selectedCount, candidateCount, canCreatePrototype } = defineProps<{
  status: string
  selectedCount: number
  candidateCount: number
  canCreatePrototype: boolean
}>()

const emit = defineEmits<{
  frame: []
  prototype: [mode: 'manual' | 'slideshow']
  openInteraction: []
}>()
</script>

<template>
  <div
    v-if="status !== 'streaming' && status !== 'submitted'"
    data-test-id="device-quick-actions"
    class="scrollbar-thin flex shrink-0 gap-1.5 overflow-x-auto px-3 pt-1.5 pb-1"
  >
    <button
      v-if="selectedCount < 2"
      data-test-id="device-quick-deploy-frame"
      type="button"
      class="flex h-7 shrink-0 items-center gap-1.5 rounded-full border border-border bg-panel-field px-2.5 text-[11px] text-surface shadow-sm hover:bg-panel-field-hover"
      @click="emit('frame')"
    >
      <icon-lucide-monitor-up class="size-3.5 text-accent" />
      烧录选中的画面
    </button>
    <button
      v-if="canCreatePrototype"
      data-test-id="device-quick-deploy-prototype"
      type="button"
      class="flex h-7 shrink-0 items-center gap-1.5 rounded-full border border-border bg-panel-field px-2.5 text-[11px] text-surface shadow-sm hover:bg-panel-field-hover"
      @click="emit('prototype', 'manual')"
    >
      <icon-lucide-git-branch class="size-3.5 text-accent" />
      手动浏览 {{ candidateCount }} 个画面
    </button>
    <button
      v-if="canCreatePrototype"
      data-test-id="device-quick-deploy-slideshow"
      type="button"
      class="flex h-7 shrink-0 items-center gap-1.5 rounded-full border border-border bg-panel-field px-2.5 text-[11px] text-surface shadow-sm hover:bg-panel-field-hover"
      @click="emit('prototype', 'slideshow')"
    >
      <icon-lucide-play class="size-3.5 text-accent" />
      自动播放 {{ candidateCount }} 个画面
    </button>
    <button
      v-if="canCreatePrototype"
      type="button"
      class="flex h-7 shrink-0 items-center gap-1.5 rounded-full border border-border bg-panel-field px-2.5 text-[11px] text-surface shadow-sm hover:bg-panel-field-hover"
      @click="emit('openInteraction')"
    >
      <icon-lucide-sliders-horizontal class="size-3.5 text-accent" />
      自定义交互
    </button>
  </div>
</template>
