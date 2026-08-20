import { Chat } from '@ai-sdk/vue'
import { DirectChatTransport, stepCountIs, ToolLoopAgent } from 'ai'
import type { ChatTransport, FinishReason, LanguageModel, UIMessage } from 'ai'
import type { ComputedRef, Ref } from 'vue'
import { ref } from 'vue'

import { ACP_AGENTS } from '@open-pencil/core/constants'
import type { ACPAgentID, AIProviderID } from '@open-pencil/core/constants'

import {
  classifyAIChatError,
  classifyAIChatFinish,
  type AIChatFailure
} from '@/app/ai/chat/failure'
import { resolveLanguageModelID } from '@/app/ai/chat/model'
import { buildReasoningProviderOptions, type AIProviderOptions } from '@/app/ai/chat/reasoning'
import SYSTEM_PROMPT from '@/app/ai/chat/system-prompt.md?raw'
import type { PrepareDevicePrototypeProposalInput } from '@/app/ai/device/prototype'
import {
  createDeviceTools,
  prepareUsbFrameDeploymentOutput,
  prepareUsbPrototypeDeploymentOutput,
  resolveEmbeddedImagePlacement
} from '@/app/ai/device/tools'
import { createAIModelRuntime, resolveModelConnectionAPIKey } from '@/app/ai/models'
import { MAX_AGENT_STEPS, createAITools, recordStepUsage, resetRunSteps } from '@/app/ai/tools'
import type { getActiveEditorStore } from '@/app/editor/active-store'
import { createEmbeddedDesignSource } from '@/app/editor/embedded-design-source'

type EditorStore = ReturnType<typeof getActiveEditorStore>

type ChatSessionOptions = {
  isConfigured: ComputedRef<boolean>
  isACPProvider: ComputedRef<boolean>
  isHarnessProvider: ComputedRef<boolean>
  providerID: Ref<AIProviderID>
  credentialsReady: Promise<void>
  getActiveEditorStore: () => EditorStore
}

type ToolLoopTransportOptions = {
  store: EditorStore
  providerID: AIProviderID
  model: LanguageModel
  effectiveModelID: string
  maxOutputTokens: number
  reasoningEffort: string
}

const ANTHROPIC_CACHE_CONTROL = {
  anthropic: { cacheControl: { type: 'ephemeral' } }
} as const

function supportsAnthropicCaching(providerID: AIProviderID, modelID: string): boolean {
  return (
    providerID === 'anthropic' ||
    providerID === 'anthropic-compatible' ||
    (providerID === 'openrouter' && modelID.startsWith('anthropic/'))
  )
}

function mergeProviderOptions(
  cacheOptions: typeof ANTHROPIC_CACHE_CONTROL | undefined,
  reasoningOptions: AIProviderOptions | undefined
): AIProviderOptions | undefined {
  if (!cacheOptions && !reasoningOptions) return undefined
  return { ...cacheOptions, ...reasoningOptions }
}

export async function createACPTransport(providerID: AIProviderID) {
  const agentId = providerID.replace('acp:', '') as ACPAgentID
  const agentDef = ACP_AGENTS.find((a) => a.id === agentId)
  if (!agentDef) throw new Error(`Unknown ACP agent: ${agentId}`)

  const { ACPChatTransport } = await import('@/app/ai/acp/transport')
  const { homeDir } = await import('@tauri-apps/api/path')
  return new ACPChatTransport({ agentDef, cwd: await homeDir() })
}

export function createToolLoopTransport({
  store,
  providerID,
  model,
  effectiveModelID,
  maxOutputTokens,
  reasoningEffort
}: ToolLoopTransportOptions) {
  const tools = {
    ...createAITools(store),
    ...createDeviceTools(createEmbeddedDesignSource(store))
  }
  const cacheProviderOptions = supportsAnthropicCaching(providerID, effectiveModelID)
    ? ANTHROPIC_CACHE_CONTROL
    : undefined
  const providerOptions = mergeProviderOptions(
    cacheProviderOptions,
    buildReasoningProviderOptions(providerID, reasoningEffort)
  )

  const agent = new ToolLoopAgent({
    model,
    instructions: SYSTEM_PROMPT,
    tools,
    stopWhen: stepCountIs(MAX_AGENT_STEPS),
    maxOutputTokens,
    providerOptions,
    prepareCall: (options) => {
      resetRunSteps(store)
      return {
        ...options,
        maxOutputTokens,
        providerOptions
      }
    },
    onStepFinish: ({ usage }) => {
      recordStepUsage(
        {
          inputTokens: usage.inputTokens ?? 0,
          outputTokens: usage.outputTokens ?? 0,
          cacheReadTokens: usage.inputTokenDetails.cacheReadTokens ?? 0,
          cacheWriteTokens: usage.inputTokenDetails.cacheWriteTokens ?? 0,
          timestamp: Date.now()
        },
        store
      )
    }
  })

  return new DirectChatTransport({ agent }) as ChatTransport<UIMessage>
}

export function createChatSessionManager({
  isConfigured,
  isACPProvider,
  isHarnessProvider,
  providerID,
  credentialsReady,
  getActiveEditorStore
}: ChatSessionOptions) {
  const failure = ref<AIChatFailure | null>(null)
  let transportDirty = false
  let currentChatStore: EditorStore | null = null
  let currentChatMessages = new WeakMap<EditorStore, UIMessage[]>()
  let localDeviceResultKeys = new WeakMap<EditorStore, Set<string>>()
  let chat: Chat<UIMessage> | null = null
  let acpTransportInstance: { destroy(): Promise<void> } | null = null
  let harnessTransportInstance: { destroy(): Promise<void> } | null = null
  let overrideTransport: (() => ChatTransport<UIMessage>) | null = null

  const localDeviceTransport: ChatTransport<UIMessage> = {
    async sendMessages() {
      return new ReadableStream()
    },
    async reconnectToStream() {
      return null
    }
  }

  function handleChatFinish({
    finishReason,
    isAbort,
    isError
  }: {
    finishReason?: FinishReason
    isAbort: boolean
    isError: boolean
  }): void {
    if (!isAbort && !isError) failure.value = classifyAIChatFinish(finishReason)
  }

  function clearFailure(): void {
    failure.value = null
  }

  function markTransportDirty() {
    transportDirty = true
    currentChatStore = null
    currentChatMessages = new WeakMap()
  }

  async function destroyAgentTransports(): Promise<void> {
    const acp = acpTransportInstance
    const harness = harnessTransportInstance
    acpTransportInstance = null
    harnessTransportInstance = null
    const results = await Promise.allSettled([acp?.destroy(), harness?.destroy()])
    const errors = results
      .filter((result) => result.status === 'rejected')
      .map((result) => result.reason)
    if (errors.length) throw new AggregateError(errors, 'Agent transport teardown failed')
  }

  async function createActiveACPTransport() {
    await destroyAgentTransports()
    const transport = await createACPTransport(providerID.value)
    acpTransportInstance = transport
    return transport as ChatTransport<UIMessage>
  }

  async function createActiveHarnessTransport() {
    await destroyAgentTransports()
    const runtime = await createAIModelRuntime('design')
    if (runtime?.kind !== 'harness') throw new Error('The Design agent is not configured for Pi')
    const [{ HarnessChatTransport }, { buildPiMCPServers }, { getActiveTabId }] = await Promise.all(
      [import('@/app/ai/harness/transport'), import('@/app/integrations/mcp'), import('@/app/tabs')]
    )
    const apiKey = await resolveModelConnectionAPIKey(runtime.role.connection.id)
    if (!apiKey) throw new Error('Credential is unavailable for the Pi agent')
    const model = runtime.role.profile.customModelID || runtime.role.profile.modelID
    const transport = new HarnessChatTransport(
      `tab-${getActiveTabId()}-${runtime.role.profile.id}`,
      {
        adapter: 'pi',
        sandbox: 'just-bash',
        model,
        settings: {
          thinkingLevel: runtime.role.profile.harnessThinkingLevel ?? 'medium',
          permissionMode: runtime.role.profile.harnessPermissionMode ?? 'allow-edits'
        },
        instructions: SYSTEM_PROMPT,
        mcpServers: await buildPiMCPServers()
      },
      { OPENPENCIL_HARNESS_API_KEY: apiKey }
    )
    harnessTransportInstance = transport
    return transport as ChatTransport<UIMessage>
  }

  async function createTransport(store: EditorStore) {
    if (overrideTransport) return overrideTransport()

    await destroyAgentTransports()

    const runtime = await createAIModelRuntime('design')
    if (runtime?.kind !== 'direct') {
      throw new Error('The Design model is not configured for direct API access')
    }
    return createToolLoopTransport({
      store,
      providerID: runtime.role.connection.providerID,
      model: runtime.model,
      effectiveModelID: resolveLanguageModelID({
        providerID: runtime.role.connection.providerID,
        modelID: runtime.role.profile.modelID,
        customModelID: runtime.role.profile.customModelID
      }),
      maxOutputTokens: runtime.role.profile.maxOutputTokens,
      reasoningEffort: runtime.role.profile.reasoningEffort ?? ''
    })
  }

  async function ensureChat(allowUnconfiguredLocal = false): Promise<Chat<UIMessage> | null> {
    await credentialsReady
    const store = getActiveEditorStore()
    const hasCurrentLocalChat = !isConfigured.value && currentChatStore === store && !!chat
    if (!isConfigured.value && !allowUnconfiguredLocal && !hasCurrentLocalChat) return null

    if (currentChatStore && chat) {
      currentChatMessages.set(currentChatStore, chat.messages)
    }

    if (!chat || transportDirty || currentChatStore !== store) {
      const messages = currentChatMessages.get(store)
      let transport: ChatTransport<UIMessage>
      if (!isConfigured.value) transport = localDeviceTransport
      else if (isACPProvider.value) transport = await createActiveACPTransport()
      else if (isHarnessProvider.value) transport = await createActiveHarnessTransport()
      else transport = await createTransport(store)
      chat = new Chat<UIMessage>({
        transport,
        messages,
        onError: (error) => {
          failure.value = classifyAIChatError(error)
        },
        onFinish: handleChatFinish
      })
      currentChatStore = store
      transportDirty = false
    }
    return chat
  }

  async function submitLocalDeviceToolAction(
    text: string,
    toolName: 'prepare_usb_frame_deployment' | 'prepare_usb_prototype_deployment',
    input: unknown,
    prepare: () => Promise<unknown>,
    successText: string
  ): Promise<Chat<UIMessage> | null> {
    const activeChat = await ensureChat(true)
    if (!activeChat) return null
    activeChat.clearError()

    const toolCallId = activeChat.generateId()
    activeChat.messages = [
      ...activeChat.messages,
      {
        id: activeChat.generateId(),
        role: 'user',
        parts: [{ type: 'text', text }]
      }
    ]

    try {
      const output = await prepare()
      activeChat.messages = [
        ...activeChat.messages,
        {
          id: activeChat.generateId(),
          role: 'assistant',
          parts: [
            {
              type: 'dynamic-tool',
              toolName,
              toolCallId,
              state: 'output-available',
              input,
              output
            },
            { type: 'text', text: successText }
          ]
        }
      ]
    } catch (error) {
      activeChat.messages = [
        ...activeChat.messages,
        {
          id: activeChat.generateId(),
          role: 'assistant',
          parts: [
            {
              type: 'dynamic-tool',
              toolName,
              toolCallId,
              state: 'output-error',
              input,
              errorText: error instanceof Error ? error.message : String(error)
            }
          ]
        }
      ]
    }
    return activeChat
  }

  async function submitLocalDeviceAction(text: string): Promise<Chat<UIMessage> | null> {
    const input = {
      intent: text,
      placement: resolveEmbeddedImagePlacement(text)
    }
    return submitLocalDeviceToolAction(
      text,
      'prepare_usb_frame_deployment',
      input,
      () =>
        prepareUsbFrameDeploymentOutput(
          createEmbeddedDesignSource(getActiveEditorStore()),
          input.intent,
          undefined,
          input.placement
        ),
      '部署参数已准备好，请检查确认卡后执行。'
    )
  }

  async function submitLocalDevicePrototypeAction(
    text: string,
    input: PrepareDevicePrototypeProposalInput
  ): Promise<Chat<UIMessage> | null> {
    return submitLocalDeviceToolAction(
      text,
      'prepare_usb_prototype_deployment',
      input,
      async () =>
        prepareUsbPrototypeDeploymentOutput(
          createEmbeddedDesignSource(getActiveEditorStore()),
          input
        ),
      '交互方案已准备好，请检查确认卡后创建并烧录。'
    )
  }

  function appendLocalDeviceResult(text: string, resultKey: string): void {
    const normalizedText = text.trim()
    if (!normalizedText || !resultKey) return
    const store = getActiveEditorStore()
    const reportedKeys = localDeviceResultKeys.get(store) ?? new Set<string>()
    if (reportedKeys.has(resultKey)) return
    reportedKeys.add(resultKey)
    localDeviceResultKeys.set(store, reportedKeys)

    const message: UIMessage = {
      id: currentChatStore === store && chat ? chat.generateId() : globalThis.crypto.randomUUID(),
      role: 'assistant',
      parts: [{ type: 'text', text: normalizedText }]
    }
    if (currentChatStore === store && chat) {
      chat.messages = [...chat.messages, message]
      return
    }
    currentChatMessages.set(store, [...(currentChatMessages.get(store) ?? []), message])
  }

  async function resetChat() {
    if (currentChatStore) {
      currentChatMessages.delete(currentChatStore)
      localDeviceResultKeys.delete(currentChatStore)
    }
    await destroyAgentTransports()
    failure.value = null
    chat = null
    currentChatStore = null
    transportDirty = false
    localDeviceResultKeys = new WeakMap()
  }

  function setOverrideTransport(factory: (() => ChatTransport<UIMessage>) | null) {
    overrideTransport = factory
    markTransportDirty()
  }

  return {
    ensureChat,
    submitLocalDeviceAction,
    submitLocalDevicePrototypeAction,
    appendLocalDeviceResult,
    resetChat,
    markTransportDirty,
    setOverrideTransport,
    failure,
    clearFailure
  }
}
