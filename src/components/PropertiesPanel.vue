<script setup lang="ts">
import { computed } from 'vue'
import { TabsContent, TabsList, TabsRoot, TabsTrigger } from 'reka-ui'

import { useI18n } from '@open-pencil/vue'
import { useAIChat } from '@/app/ai/chat/use'
import { useEditorStore } from '@/app/editor/active-store'
import {
  bakeDevicePrototypeAnimation,
  bakeDevicePrototypeFromSource,
  createDevicePrototypeFrameRendererFromSource,
  getDevicePrototypeFrameCandidateFromSource,
  getSelectedDevicePrototypeFrameCandidatesFromSource
} from '@/app/editor/device-prototype'
import {
  bakeEmbeddedFrameByIdFromSource,
  bakeEmbeddedFrameFromSource,
  getEmbeddedFrameBakeStateFromSource
} from '@/app/editor/embedded-display-bake'
import { createEmbeddedDesignSource } from '@/app/editor/embedded-design-source'
import { createPresetFrameName } from '@/app/editor/preset-frame-name'
import { DevicePrototypePanel, useDevicePrototype } from '@/features/device-prototype'
import { EmbeddedDisplayPanel } from '@/features/embedded-display'

import ChatPanel from './ChatPanel.vue'
import CodePanel from './CodePanel.vue'
import DesignPanel from './DesignPanel.vue'
import ZoomDropdown from './editor/ZoomDropdown.vue'

const { activeTab } = useAIChat()
const { panels } = useI18n()
const editorStore = useEditorStore()
const embeddedDesignSource = createEmbeddedDesignSource(editorStore)
const devicePrototypeFrame = computed(() =>
  getDevicePrototypeFrameCandidateFromSource(embeddedDesignSource)
)
const selectedDevicePrototypeFrames = computed(() =>
  getSelectedDevicePrototypeFrameCandidatesFromSource(embeddedDesignSource)
)
const devicePrototypeFrameRenderer =
  createDevicePrototypeFrameRendererFromSource(embeddedDesignSource)
const { interactionOptions, interactions } = useDevicePrototype(embeddedDesignSource)
const embeddedBakeState = computed(() => getEmbeddedFrameBakeStateFromSource(embeddedDesignSource))

async function handleEmbeddedFrameBake() {
  return bakeEmbeddedFrameFromSource(embeddedDesignSource)
}

async function handleEmbeddedFrameBakeById(frameId: string) {
  return bakeEmbeddedFrameByIdFromSource(embeddedDesignSource, frameId)
}

async function handleEmbeddedPrototypeBake(interactionId: string) {
  const interaction = interactions.value.find((item) => item.id === interactionId)
  return interaction ? bakeDevicePrototypeFromSource(embeddedDesignSource, interaction) : null
}

function handleAnimatedPrototypeBake(interactionId: string) {
  const interaction = interactions.value.find((item) => item.id === interactionId)
  return interaction ? bakeDevicePrototypeAnimation(interaction) : null
}

function handleCreateEmbeddedPresetFrame(width: number, height: number, profileName: string) {
  const viewportCenter = editorStore.viewportCanvasCenter()
  const center = editorStore.screenToCanvas(viewportCenter.x, viewportCenter.y)
  const frameId = editorStore.createShape(
    'FRAME',
    center.x - width / 2,
    center.y - height / 2,
    width,
    height
  )
  const existingFrameNames = editorStore.graph
    .getChildren(editorStore.state.currentPageId)
    .filter((node) => node.type === 'FRAME' && node.id !== frameId)
    .map((node) => node.name)
  editorStore.renameNode(frameId, createPresetFrameName(profileName, existingFrameNames))
  editorStore.select([frameId])
  editorStore.requestRender()
}
</script>

<template>
  <aside
    data-test-id="properties-panel"
    class="flex min-w-0 flex-1 flex-col overflow-hidden border-l border-border bg-panel"
    style="contain: paint layout style"
  >
    <TabsRoot v-model="activeTab" class="flex min-h-0 flex-1 flex-col">
      <TabsList class="flex h-10 shrink-0 items-center gap-1 border-b border-border px-2">
        <TabsTrigger
          value="design"
          data-test-id="properties-tab-design"
          class="relative rounded px-2.5 py-1 text-[11px] text-muted hover:text-surface data-[state=active]:font-semibold data-[state=active]:text-surface after:absolute after:inset-x-2 after:-bottom-[9px] after:h-0.5 after:rounded-full after:bg-transparent data-[state=active]:after:bg-accent"
        >
          {{ panels.design }}
        </TabsTrigger>
        <TabsTrigger
          value="code"
          data-test-id="properties-tab-code"
          class="relative flex items-center gap-1 rounded px-2.5 py-1 text-[11px] text-muted hover:text-surface data-[state=active]:font-semibold data-[state=active]:text-surface after:absolute after:inset-x-2 after:-bottom-[9px] after:h-0.5 after:rounded-full after:bg-transparent data-[state=active]:after:bg-accent"
        >
          <icon-lucide-code class="size-3" />
          {{ panels.code }}
        </TabsTrigger>
        <TabsTrigger
          value="ai"
          data-test-id="properties-tab-ai"
          class="relative flex items-center gap-1 rounded px-2.5 py-1 text-[11px] text-muted hover:text-surface data-[state=active]:font-semibold data-[state=active]:text-surface after:absolute after:inset-x-2 after:-bottom-[9px] after:h-0.5 after:rounded-full after:bg-transparent data-[state=active]:after:bg-accent"
        >
          <icon-lucide-sparkles class="size-3" />
          {{ panels.ai }}
        </TabsTrigger>
        <TabsTrigger
          value="prototype"
          data-test-id="properties-tab-prototype"
          class="relative rounded px-2.5 py-1 text-[11px] text-muted hover:text-surface data-[state=active]:font-semibold data-[state=active]:text-surface after:absolute after:inset-x-2 after:-bottom-[9px] after:h-0.5 after:rounded-full data-[state=active]:after:bg-accent"
        >
          交互
        </TabsTrigger>
        <TabsTrigger
          value="embedded"
          data-test-id="properties-tab-embedded"
          class="relative rounded px-2.5 py-1 text-[11px] text-muted hover:text-surface data-[state=active]:font-semibold data-[state=active]:text-surface after:absolute after:inset-x-2 after:-bottom-[9px] after:h-0.5 after:rounded-full data-[state=active]:after:bg-accent"
        >
          烧录
        </TabsTrigger>
        <ZoomDropdown v-if="activeTab === 'design'" />
      </TabsList>

      <TabsContent
        value="design"
        class="flex min-h-0 flex-1 flex-col"
        :force-mount="true"
        :hidden="activeTab !== 'design'"
      >
        <DesignPanel />
      </TabsContent>

      <TabsContent
        value="code"
        class="flex min-h-0 flex-1 flex-col"
        :force-mount="true"
        :hidden="activeTab !== 'code'"
      >
        <CodePanel :active="activeTab === 'code'" />
      </TabsContent>

      <TabsContent
        value="ai"
        class="flex min-h-0 flex-1 flex-col"
        :force-mount="true"
        :hidden="activeTab !== 'ai'"
      >
        <ChatPanel />
      </TabsContent>

      <TabsContent
        value="prototype"
        class="flex min-h-0 flex-1 flex-col"
        :force-mount="true"
        :hidden="activeTab !== 'prototype'"
      >
        <DevicePrototypePanel
          :active="activeTab === 'prototype'"
          :scope-key="embeddedDesignSource"
          :selected-frame="devicePrototypeFrame"
          :selected-frames="selectedDevicePrototypeFrames"
          :render-frame="devicePrototypeFrameRenderer"
          :render-revision="editorStore.state.sceneVersion"
          @open-flashing="activeTab = 'embedded'"
        />
      </TabsContent>

      <TabsContent
        value="embedded"
        class="flex min-h-0 flex-1 flex-col"
        :force-mount="true"
        :hidden="activeTab !== 'embedded'"
      >
        <EmbeddedDisplayPanel
          :bake-state="embeddedBakeState"
          :bake-frame="handleEmbeddedFrameBake"
          :bake-frame-by-id="handleEmbeddedFrameBakeById"
          :prototype-options="interactionOptions"
          :prototype-interactions="interactions"
          :render-prototype-frame="devicePrototypeFrameRenderer"
          :prototype-render-revision="editorStore.state.sceneVersion"
          :bake-prototype="handleEmbeddedPrototypeBake"
          :bake-animation="handleAnimatedPrototypeBake"
          :create-preset-frame="handleCreateEmbeddedPresetFrame"
        />
      </TabsContent>
    </TabsRoot>
  </aside>
</template>
