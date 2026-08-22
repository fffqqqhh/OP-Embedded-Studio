<script setup lang="ts">
import { computed } from 'vue'
import { isTextUIPart, isToolUIPart } from 'ai'
import { Markdown } from 'vue-stream-markdown'
import { vTestId } from '@open-pencil/vue'
import 'vue-stream-markdown/index.css'

import {
  imageAttachmentsForMessage,
  visibleUserMessageText
} from '@/app/ai/attachment/image/presentation'
import { resolvedAppTheme } from '@/app/shell/theme'
import ImageAttachment from '@/components/chat/attachment/image/ImageAttachment.vue'
import ChatAssistantItem from '@/components/chat/ChatAssistantItem.vue'

import type { UIDataTypes, UIMessage, UIMessagePart, UITools } from 'ai'

const { message, streaming = false } = defineProps<{
  message: UIMessage
  streaming?: boolean
}>()
const isDark = computed(() => resolvedAppTheme.value === 'dark')
const markdownMode = computed(() => (streaming ? 'streaming' : 'static'))
const imageAttachments = imageAttachmentsForMessage(message.id)

function partKey(part: UIMessagePart<UIDataTypes, UITools>, index: number): string {
  if ('toolCallId' in part) return part.toolCallId
  return `part-${index}`
}
</script>

<template>
  <div
    v-test-id="`chat-message-${message.role}`"
    :class="message.role === 'user' ? 'flex justify-end' : ''"
  >
    <div
      class="min-w-0 space-y-2 select-text"
      :class="message.role === 'user' ? 'max-w-[85%]' : ''"
    >
      <template v-if="message.role === 'assistant'">
        <template v-for="(part, i) in message.parts" :key="partKey(part, i)">
          <!-- Tool call -->
          <ChatAssistantItem
            v-if="isToolUIPart(part)"
            :item="{ kind: 'part', key: partKey(part, i), part }"
          />

          <!-- Text -->
          <div
            v-else-if="isTextUIPart(part) && part.text"
            data-test-id="chat-text-bubble"
            class="rounded-xl rounded-tl-md bg-hover px-3 py-2 text-xs leading-relaxed text-surface"
          >
            <Markdown
              :key="markdownMode"
              :content="part.text"
              :is-dark="isDark"
              :mermaid="false"
              :mode="markdownMode"
              :data-chat-markdown-mode="markdownMode"
              class="chat-markdown [&_[data-stream-markdown=code]]:!bg-input"
            />
          </div>
        </template>
      </template>

      <!-- User message -->
      <template v-else-if="message.role === 'user'">
        <div v-if="imageAttachments.length" class="flex flex-wrap justify-end gap-1.5">
          <ImageAttachment
            v-for="attachment in imageAttachments"
            :key="attachment.id"
            :attachment="attachment"
          />
        </div>
        <div
          data-test-id="chat-text-bubble"
          class="rounded-xl rounded-br-md bg-accent px-3 py-2 text-xs leading-relaxed whitespace-pre-wrap text-white"
        >
          {{
            visibleUserMessageText(
              message.id,
              message.parts
                .filter(isTextUIPart)
                .map((p) => p.text)
                .join('')
            )
          }}
        </div>
      </template>
    </div>
  </div>
</template>
