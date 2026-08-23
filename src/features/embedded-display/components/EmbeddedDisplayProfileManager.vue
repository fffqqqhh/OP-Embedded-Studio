<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue'

import { AppDialogBody, AppDialogFooter, AppDialogHeader, AppDialogRoot } from '@/components/ui/dialog'

import type { EmbeddedDisplayProfile } from '../model/types'
import { useEmbeddedDisplay } from '../composables/useEmbeddedDisplay'

const props = defineProps<{ editProfileId?: string }>()
const open = defineModel<boolean>('open', { default: false })
const emit = defineEmits<{ saved: [profile: EmbeddedDisplayProfile] }>()

type GpioRow = NonNullable<EmbeddedDisplayProfile['gpio']>[number]
interface ProfileForm {
  id: string
  name: string
  module: string
  controller: string
  driverIc: string
  interface: string
  width: number
  height: number
  shape: 'round' | 'rectangle'
  colorOrder: 'RGB' | 'BGR'
  byteOrder: 'little' | 'big'
  transport: string
  flashSize: string
  contentBytes: number | undefined
  gpio: GpioRow[]
}

function blankForm(): ProfileForm {
  return {
    id: '',
    name: '',
    module: '',
    controller: '',
    driverIc: '',
    interface: '4-wire SPI',
    width: 466,
    height: 466,
    shape: 'round',
    colorOrder: 'RGB',
    byteOrder: 'little',
    transport: 'SPI',
    flashSize: '',
    contentBytes: undefined,
    gpio: [
      { signal: 'SCLK', gpio: 'GPIO12', note: '' },
      { signal: 'MOSI', gpio: 'GPIO11', note: '' },
      { signal: 'DC', gpio: 'GPIO9', note: '' },
      { signal: 'RESET', gpio: 'GPIO14', note: '' },
      { signal: 'CS', gpio: 'GPIO10', note: '' }
    ]
  }
}

const { profiles, saveCustomProfile, deleteCustomProfile, exportCustomProfiles, importCustomProfiles } =
  useEmbeddedDisplay()
const form = reactive<ProfileForm>(blankForm())
const message = ref('')
const error = ref('')
const importInput = ref<HTMLInputElement>()
const customProfiles = computed(() => profiles.value.filter((profile) => profile.source === 'custom'))
const editing = computed(() => Boolean(form.id))

function copyForm(next: ProfileForm): void {
  Object.assign(form, next)
  form.gpio = next.gpio.map((row) => ({ ...row }))
}

function profileToForm(profile: EmbeddedDisplayProfile): ProfileForm {
  return {
    id: profile.id,
    name: profile.name,
    module: profile.module ?? '',
    controller: profile.controller,
    driverIc: profile.driverIc ?? profile.controller,
    interface: profile.interface,
    width: profile.resolution.width,
    height: profile.resolution.height,
    shape: profile.visibleArea?.shape === 'round' ? 'round' : 'rectangle',
    colorOrder: profile.image?.colorOrder === 'BGR' ? 'BGR' : 'RGB',
    byteOrder: profile.image?.byteOrder === 'big' ? 'big' : 'little',
    transport: profile.image?.transport ?? profile.interface,
    flashSize: profile.flashSize ?? '',
    contentBytes: profile.wirelessContentBytes,
    gpio: profile.gpio?.length ? profile.gpio.map((row) => ({ ...row })) : [{ signal: '', gpio: '', note: '' }]
  }
}

function beginCreate(): void {
  copyForm(blankForm())
  message.value = ''
  error.value = ''
}

function beginEdit(profile: EmbeddedDisplayProfile): void {
  copyForm(profileToForm(profile))
  message.value = ''
  error.value = ''
}

function openImportPicker(): void {
  importInput.value?.click()
}

async function handleImport(event: Event): Promise<void> {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  input.value = ''
  if (!file) return
  try {
    const imported = JSON.parse(await file.text()) as unknown
    const profiles = importCustomProfiles(imported)
    message.value = `已导入 ${profiles.length} 个自定义方案`
    error.value = ''
  } catch (reason) {
    error.value = reason instanceof Error ? reason.message : '配置文件读取失败'
  }
}

function downloadExport(): void {
  const blob = new Blob([JSON.stringify(exportCustomProfiles(), null, 2)], { type: 'application/json' })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = 'op-embedded-screen-profiles.json'
  link.click()
  URL.revokeObjectURL(url)
  message.value = '配置已导出'
}

function addGpioRow(): void {
  form.gpio.push({ signal: '', gpio: '', note: '' })
}

function removeGpioRow(index: number): void {
  form.gpio.splice(index, 1)
}

function save(): void {
  error.value = ''
  const name = form.name.trim()
  const controller = form.controller.trim()
  if (!name || !controller || !form.width || !form.height) {
    error.value = '请至少填写方案名称、驱动控制器和分辨率'
    return
  }
  const id = form.id || `custom-${Date.now().toString(36)}`
  try {
    const profile = saveCustomProfile({
      id,
      name,
      controller,
      module: form.module.trim() || undefined,
      driverIc: form.driverIc.trim() || controller,
      resolution: { width: Math.round(form.width), height: Math.round(form.height) },
      interface: form.interface.trim() || '4-wire SPI',
      backgroundColor: '#000000',
      description: '用户自定义屏幕方案；需要匹配的预编译固件。',
      verified: false,
      flashSize: form.flashSize.trim() || undefined,
      wirelessContentBytes: form.contentBytes || undefined,
      visibleArea: { shape: form.shape },
      image: {
        pixelFormat: 'RGB565',
        colorOrder: form.colorOrder,
        byteOrder: form.byteOrder,
        rotation: 0,
        xGap: 0,
        yGap: 0,
        transport: form.transport.trim() || form.interface.trim() || 'SPI'
      },
      gpio: form.gpio.filter((row) => row.signal.trim() && row.gpio.trim()).map((row) => ({
        signal: row.signal.trim(),
        gpio: row.gpio.trim(),
        pin: row.pin?.trim() || undefined,
        note: row.note?.trim() || undefined
      })),
      source: 'custom',
      firmwareAvailable: false
    })
    emit('saved', profile)
    message.value = '方案已保存到本机'
    copyForm(profileToForm(profile))
  } catch (reason) {
    error.value = reason instanceof Error ? reason.message : '方案保存失败'
  }
}

function remove(profile: EmbeddedDisplayProfile): void {
  if (!window.confirm(`确定删除“${profile.name}”吗？`)) return
  deleteCustomProfile(profile.id)
  if (form.id === profile.id) beginCreate()
  message.value = '方案已删除'
}

watch(
  () => [open.value, props.editProfileId] as const,
  ([isOpen, profileId]) => {
    if (!isOpen) return
    const profile = profileId ? profiles.value.find((item) => item.id === profileId) : undefined
    if (profile?.source === 'custom') beginEdit(profile)
    else beginCreate()
  },
  { immediate: true }
)
</script>

<template>
  <AppDialogRoot v-model:open="open" size="lg" height="tall">
    <AppDialogHeader
      heading="屏幕方案管理"
      description="保存分辨率、驱动、颜色和 GPIO 接线；自定义方案需要匹配的预编译固件。"
    />
    <AppDialogBody class="min-h-0 space-y-4 overflow-y-auto">
      <div class="flex items-center justify-between gap-2 border-b border-border pb-3">
        <div>
          <p class="text-[11px] font-semibold text-surface">我的方案</p>
          <p class="mt-0.5 text-[10px] text-muted">配置保存在当前浏览器，可导出到其他设备。</p>
        </div>
        <div class="flex items-center gap-1.5">
          <button type="button" class="rounded-panel border border-border px-2 py-1.5 text-[10px] text-surface hover:bg-hover" @click="beginCreate">
            <icon-lucide-plus class="mr-1 inline size-3" />新建
          </button>
          <button type="button" class="rounded-panel border border-border px-2 py-1.5 text-[10px] text-surface hover:bg-hover" @click="openImportPicker">
            <icon-lucide-upload class="mr-1 inline size-3" />导入
          </button>
          <button type="button" class="rounded-panel border border-border px-2 py-1.5 text-[10px] text-surface hover:bg-hover" :disabled="!customProfiles.length" @click="downloadExport">
            <icon-lucide-download class="mr-1 inline size-3" />导出
          </button>
          <input ref="importInput" type="file" accept="application/json,.json" class="hidden" @change="handleImport" />
        </div>
      </div>

      <div v-if="customProfiles.length" class="grid gap-1.5">
        <div v-for="profile in customProfiles" :key="profile.id" class="flex items-center justify-between gap-2 rounded-panel border border-border bg-panel-field px-2.5 py-2">
          <div class="min-w-0">
            <p class="truncate text-[11px] font-medium text-surface">{{ profile.name }}</p>
            <p class="truncate text-[9px] text-muted">{{ profile.resolution.width }} × {{ profile.resolution.height }} · {{ profile.driverIc }} · 自定义</p>
          </div>
          <div class="flex shrink-0 items-center gap-1">
            <button type="button" class="rounded p-1.5 text-muted hover:bg-hover hover:text-surface" title="编辑" @click="beginEdit(profile)"><icon-lucide-pencil class="size-3.5" /></button>
            <button type="button" class="rounded p-1.5 text-muted hover:bg-hover hover:text-danger" title="删除" @click="remove(profile)"><icon-lucide-trash-2 class="size-3.5" /></button>
          </div>
        </div>
      </div>
      <p v-else class="rounded-panel border border-dashed border-border px-3 py-2.5 text-[10px] text-muted">还没有自定义方案，点击“新建”开始配置。</p>

      <div class="grid gap-3 rounded-panel border border-border bg-panel-field p-3">
        <div class="flex items-center justify-between">
          <div>
            <p class="text-[11px] font-semibold text-surface">{{ editing ? '编辑方案' : '新建方案' }}</p>
            <p class="mt-0.5 text-[9px] text-muted">参数用于画面预览和内容编码，不会自动生成固件。</p>
          </div>
          <span class="rounded-full bg-warning/15 px-2 py-1 text-[9px] text-warning">需要预编译固件</span>
        </div>

        <div class="grid grid-cols-2 gap-2">
          <label class="grid gap-1 text-[10px] text-muted"><span>方案名称</span><input v-model="form.name" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" placeholder="例如：我的 240 圆屏" /></label>
          <label class="grid gap-1 text-[10px] text-muted"><span>屏幕模块</span><input v-model="form.module" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" placeholder="例如：自定义 LCD 模块" /></label>
          <label class="grid gap-1 text-[10px] text-muted"><span>驱动控制器</span><input v-model="form.controller" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" placeholder="例如：ST7789" /></label>
          <label class="grid gap-1 text-[10px] text-muted"><span>驱动型号</span><input v-model="form.driverIc" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" placeholder="例如：ST7789P3" /></label>
        </div>

        <div class="grid grid-cols-2 gap-2">
          <label class="grid gap-1 text-[10px] text-muted"><span>宽度（px）</span><input v-model.number="form.width" type="number" min="1" max="8192" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" /></label>
          <label class="grid gap-1 text-[10px] text-muted"><span>高度（px）</span><input v-model.number="form.height" type="number" min="1" max="8192" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" /></label>
          <label class="grid gap-1 text-[10px] text-muted"><span>屏幕形状</span><select v-model="form.shape" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none"><option value="round">圆屏</option><option value="rectangle">矩形 / 方屏</option></select></label>
          <label class="grid gap-1 text-[10px] text-muted"><span>接口</span><input v-model="form.interface" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" placeholder="4-wire SPI" /></label>
        </div>

        <div class="grid grid-cols-3 gap-2">
          <label class="grid gap-1 text-[10px] text-muted"><span>颜色顺序</span><select v-model="form.colorOrder" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none"><option value="RGB">RGB</option><option value="BGR">BGR</option></select></label>
          <label class="grid gap-1 text-[10px] text-muted"><span>字节序</span><select v-model="form.byteOrder" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none"><option value="little">Little</option><option value="big">Big</option></select></label>
          <label class="grid gap-1 text-[10px] text-muted"><span>传输总线</span><input v-model="form.transport" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" placeholder="SPI / QSPI" /></label>
        </div>

        <div class="grid grid-cols-2 gap-2">
          <label class="grid gap-1 text-[10px] text-muted"><span>Flash 容量</span><input v-model="form.flashSize" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" placeholder="例如：16MB" /></label>
          <label class="grid gap-1 text-[10px] text-muted"><span>内容分区（字节，可选）</span><input v-model.number="form.contentBytes" type="number" min="0" class="h-control rounded-panel border border-border bg-canvas px-2 text-[11px] text-surface outline-none focus:border-accent" placeholder="例如：13565952" /></label>
        </div>

        <div class="grid gap-2">
          <div class="flex items-center justify-between"><div><p class="text-[11px] font-semibold text-surface">GPIO 接线</p><p class="text-[9px] text-muted">按固件驱动要求填写信号和 GPIO，不确定时不要直接烧录。</p></div><button type="button" class="rounded-panel border border-border px-2 py-1 text-[10px] text-surface hover:bg-hover" @click="addGpioRow"><icon-lucide-plus class="mr-1 inline size-3" />增加</button></div>
          <div v-for="(row, index) in form.gpio" :key="index" class="grid grid-cols-[1fr_1fr_1.4fr_auto] gap-1.5">
            <input v-model="row.signal" class="h-control min-w-0 rounded-panel border border-border bg-canvas px-2 text-[10px] text-surface outline-none focus:border-accent" placeholder="信号" />
            <input v-model="row.gpio" class="h-control min-w-0 rounded-panel border border-border bg-canvas px-2 text-[10px] text-surface outline-none focus:border-accent" placeholder="GPIO12" />
            <input v-model="row.note" class="h-control min-w-0 rounded-panel border border-border bg-canvas px-2 text-[10px] text-surface outline-none focus:border-accent" placeholder="说明（可选）" />
            <button type="button" class="rounded p-1.5 text-muted hover:bg-hover hover:text-danger" title="删除接线" @click="removeGpioRow(index)"><icon-lucide-trash-2 class="size-3.5" /></button>
          </div>
        </div>
      </div>
      <p v-if="message" class="text-[10px] text-success">{{ message }}</p>
      <p v-if="error" class="text-[10px] text-danger">{{ error }}</p>
    </AppDialogBody>
    <AppDialogFooter>
      <span class="mr-auto text-[9px] text-muted">自定义方案仅保存配置；可用固件仍需单独预编译。</span>
      <button type="button" class="rounded-panel border border-border px-3 py-1.5 text-[10px] text-surface hover:bg-hover" @click="open = false">关闭</button>
      <button type="button" class="rounded-panel bg-accent px-3 py-1.5 text-[10px] font-medium text-white hover:brightness-110" @click="save"><icon-lucide-save class="mr-1 inline size-3" />保存方案</button>
    </AppDialogFooter>
  </AppDialogRoot>
</template>
