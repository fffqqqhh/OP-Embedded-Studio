<script setup lang="ts">
import { computed, nextTick, onUnmounted, reactive, ref, useTemplateRef, watch } from 'vue'

import { useEventListener } from '@vueuse/core'

import type {
  DevicePrototypeInteraction,
  DevicePrototypePortDirection,
  DevicePrototypeState,
  DevicePrototypeTransition
} from '../model/types'
import { DEVICE_PROTOTYPE_EVENTS } from '../model/types'

type GraphTransition = DevicePrototypeTransition & {
  label?: string
  selectable?: boolean
}

interface Point {
  x: number
  y: number
}

interface Segment {
  from: Point
  to: Point
}

interface GraphLine {
  transitions: GraphTransition[]
  labelTransitions: GraphTransition[]
  selection: GraphTransition
  key: string
  path: string
  segments: Segment[]
  label: Point
  bidirectional: boolean
  selected: boolean
}

interface PinchState {
  distance: number
  center: Point
  zoom: number
  pan: Point
}

const NODE_HEIGHT = 34
const NODE_MIN_WIDTH = 88
const NODE_MAX_WIDTH = 192
const GRID_X = 168
const GRID_Y = 82
const GRID_SIZE = 16
const PORT_GAP = 22
const CORNER_RADIUS = 8
const ROUTE_CLEARANCE = 14
const ZOOM_MIN = 0.25
const ZOOM_MAX = 2.5

const { states, initialStateId, selectedStateId, selectedTransitionKey, transitions, interaction } =
  defineProps<{
    states: DevicePrototypeState[]
    initialStateId: string
    selectedStateId: string
    selectedTransitionKey: string
    transitions: GraphTransition[]
    interaction: DevicePrototypeInteraction | null
  }>()

const emit = defineEmits<{
  'select-state': [stateId: string]
  'select-transition': [transition: DevicePrototypeTransition]
  connect: [
    fromStateId: string,
    toStateId: string,
    fromPort: DevicePrototypePortDirection,
    toPort: DevicePrototypePortDirection
  ]
  'remove-transition': [transition: DevicePrototypeTransition]
}>()

const positions = reactive<Record<string, Point>>({})
const graphViewport = useTemplateRef<HTMLElement>('graphViewport')
const zoom = ref(1)
const pan = reactive<Point>({ x: 40, y: 44 })
const connecting = ref<{
  fromStateId: string
  direction: DevicePrototypePortDirection
  point: Point
} | null>(null)
const cutting = ref<{ points: Point[]; removed: Set<string> } | null>(null)
const zPressed = ref(false)
const touchPointers = new Map<number, Point>()

let dragState: { stateId: string; offset: Point } | null = null
let panState: { point: Point; pan: Point } | null = null
let pinchState: PinchState | null = null
let hasInitialized = false
const inferredPortPairs = new Map<
  string,
  { fromPort: DevicePrototypePortDirection; toPort: DevicePrototypePortDirection }
>()

const stateSignature = computed(() => states.map((state) => state.id).join('|'))
const worldTransform = computed(() => `translate(${pan.x}px, ${pan.y}px) scale(${zoom.value})`)
const portDirections: DevicePrototypePortDirection[] = ['top', 'right', 'bottom', 'left']
const gridStyle = computed(() => {
  const spacing = GRID_SIZE * zoom.value
  return {
    backgroundPosition: `${pan.x % spacing}px ${pan.y % spacing}px`,
    backgroundSize: `${spacing}px ${spacing}px`
  }
})

function nodeWidth(state: DevicePrototypeState): number {
  return Math.min(NODE_MAX_WIDTH, Math.max(NODE_MIN_WIDTH, state.name.length * 11 + 28))
}

function transitionKey(transition: DevicePrototypeTransition): string {
  return `${transition.fromStateId}:${transition.event}:${transition.toStateId}`
}

function transitionLabel(transition: GraphTransition): string {
  return (
    transition.label ??
    DEVICE_PROTOTYPE_EVENTS.find((event) => event.id === transition.event)?.label ??
    transition.event
  )
}

function pairKey(fromStateId: string, toStateId: string): string {
  return [fromStateId, toStateId].sort().join(':')
}

function defaultPosition(index: number): Point {
  return {
    x: (index % 3) * GRID_X,
    y: Math.floor(index / 3) * GRID_Y
  }
}

function syncPositions() {
  states.forEach((state, index) => {
    if (!positions[state.id]) positions[state.id] = defaultPosition(index)
  })
}

function graphPoint(event: PointerEvent | WheelEvent): Point | null {
  const rect = graphViewport.value?.getBoundingClientRect()
  if (!rect) return null
  return {
    x: (event.clientX - rect.left - pan.x) / zoom.value,
    y: (event.clientY - rect.top - pan.y) / zoom.value
  }
}

function viewportPoint(clientX: number, clientY: number): Point | null {
  const rect = graphViewport.value?.getBoundingClientRect()
  if (!rect) return null
  return { x: clientX - rect.left, y: clientY - rect.top }
}

function clampZoom(value: number): number {
  return Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, value))
}

function zoomAround(nextZoom: number, center: Point) {
  const value = clampZoom(nextZoom)
  const ratio = value / zoom.value
  pan.x = center.x - (center.x - pan.x) * ratio
  pan.y = center.y - (center.y - pan.y) * ratio
  zoom.value = value
}

function portPoint(
  state: DevicePrototypeState,
  position: Point,
  direction: DevicePrototypePortDirection
): Point {
  const width = nodeWidth(state)
  if (direction === 'top') return { x: position.x + width / 2, y: position.y }
  if (direction === 'bottom') return { x: position.x + width / 2, y: position.y + NODE_HEIGHT }
  if (direction === 'left') return { x: position.x, y: position.y + NODE_HEIGHT / 2 }
  return { x: position.x + width, y: position.y + NODE_HEIGHT / 2 }
}

function movePoint(point: Point, direction: DevicePrototypePortDirection, distance: number): Point {
  if (direction === 'top') return { x: point.x, y: point.y - distance }
  if (direction === 'bottom') return { x: point.x, y: point.y + distance }
  if (direction === 'left') return { x: point.x - distance, y: point.y }
  return { x: point.x + distance, y: point.y }
}

function moveLanePoint(
  point: Point,
  direction: DevicePrototypePortDirection,
  offset: number
): Point {
  if (direction === 'top' || direction === 'bottom') return { x: point.x + offset, y: point.y }
  return { x: point.x, y: point.y + offset }
}

function directionTowards(from: Point, to: Point): DevicePrototypePortDirection {
  const dx = to.x - from.x
  const dy = to.y - from.y
  if (Math.abs(dx) >= Math.abs(dy)) return dx >= 0 ? 'right' : 'left'
  return dy >= 0 ? 'bottom' : 'top'
}

function routeDirections(
  transition: GraphTransition,
  fromCenter: Point,
  toCenter: Point
): { fromPort: DevicePrototypePortDirection; toPort: DevicePrototypePortDirection } {
  if (transition.fromPort && transition.toPort) {
    return { fromPort: transition.fromPort, toPort: transition.toPort }
  }
  const key = transitionKey(transition)
  const cached = inferredPortPairs.get(key)
  if (cached) return cached
  const ports = {
    fromPort: transition.fromPort ?? directionTowards(fromCenter, toCenter),
    toPort: transition.toPort ?? directionTowards(toCenter, fromCenter)
  }
  inferredPortPairs.set(key, ports)
  return ports
}

function pointsEqual(a: Point, b: Point): boolean {
  return Math.abs(a.x - b.x) < 0.01 && Math.abs(a.y - b.y) < 0.01
}

function compactPoints(points: Point[]): Point[] {
  const result: Point[] = []
  points.forEach((point) => {
    if (result.length && pointsEqual(result[result.length - 1], point)) return
    result.push(point)
    while (result.length >= 3) {
      const a = result[result.length - 3]
      const b = result[result.length - 2]
      const c = result[result.length - 1]
      if ((a.x === b.x && b.x === c.x) || (a.y === b.y && b.y === c.y)) result.splice(-2, 1)
      else break
    }
  })
  return result
}

function roundedPath(points: Point[]): string {
  if (!points.length) return ''
  if (points.length === 1) return `M ${points[0].x} ${points[0].y}`
  let path = `M ${points[0].x} ${points[0].y}`
  for (let index = 1; index < points.length - 1; index += 1) {
    const previous = points[index - 1]
    const current = points[index]
    const next = points[index + 1]
    const incoming = Math.hypot(current.x - previous.x, current.y - previous.y)
    const outgoing = Math.hypot(next.x - current.x, next.y - current.y)
    const radius = Math.min(CORNER_RADIUS, incoming / 2, outgoing / 2)
    const before = {
      x: current.x + ((previous.x - current.x) * radius) / incoming,
      y: current.y + ((previous.y - current.y) * radius) / incoming
    }
    const after = {
      x: current.x + ((next.x - current.x) * radius) / outgoing,
      y: current.y + ((next.y - current.y) * radius) / outgoing
    }
    path += ` L ${before.x} ${before.y} Q ${current.x} ${current.y} ${after.x} ${after.y}`
  }
  const last = points[points.length - 1]
  return `${path} L ${last.x} ${last.y}`
}

function lineLabelPoint(points: Point[], labelSlot = 0): Point {
  const segments = points.slice(1).map((point, index) => ({ from: points[index], to: point }))
  let longest = segments[0]
  let longestLength = longest ? Math.hypot(longest.to.x - longest.from.x, longest.to.y - longest.from.y) : 0
  for (const segment of segments.slice(1)) {
    const length = Math.hypot(segment.to.x - segment.from.x, segment.to.y - segment.from.y)
    if (length > longestLength) {
      longest = segment
      longestLength = length
    }
  }
  if (!longest) return points[0] ?? { x: 0, y: 0 }
  const horizontal = Math.abs(longest.to.x - longest.from.x) >= Math.abs(longest.to.y - longest.from.y)
  const side = labelSlot % 2 === 0 ? -1 : 1
  const distance = 14 + Math.floor(labelSlot / 2) * 22
  return {
    x: (longest.from.x + longest.to.x) / 2 + (horizontal ? 0 : side * distance),
    y: (longest.from.y + longest.to.y) / 2 + (horizontal ? side * distance : 0)
  }
}

function routeBetween(
  transition: GraphTransition,
  fromState: DevicePrototypeState,
  toState: DevicePrototypeState,
  laneOffset = 0
): Point[] | null {
  const fromPosition = positions[fromState.id]
  const toPosition = positions[toState.id]
  if (!fromPosition || !toPosition) return null
  const fromCenter = {
    x: fromPosition.x + nodeWidth(fromState) / 2,
    y: fromPosition.y + NODE_HEIGHT / 2
  }
  const toCenter = { x: toPosition.x + nodeWidth(toState) / 2, y: toPosition.y + NODE_HEIGHT / 2 }
  const { fromPort: fromDirection, toPort: toDirection } = routeDirections(
    transition,
    fromCenter,
    toCenter
  )
  const source = portPoint(fromState, fromPosition, fromDirection)
  const target = portPoint(toState, toPosition, toDirection)
  const sourceExit = movePoint(source, fromDirection, PORT_GAP)
  const targetExit = movePoint(target, toDirection, PORT_GAP)
  const laneSource = moveLanePoint(sourceExit, fromDirection, laneOffset)
  const laneTarget = moveLanePoint(targetExit, toDirection, laneOffset)
  const horizontal = (direction: DevicePrototypePortDirection) =>
    direction === 'left' || direction === 'right'

  if (horizontal(fromDirection) && horizontal(toDirection)) {
    const sameDirection = fromDirection === toDirection
    let laneX = (laneSource.x + laneTarget.x) / 2
    if (sameDirection && fromDirection === 'left') {
      laneX = Math.min(fromPosition.x, toPosition.x) - PORT_GAP - ROUTE_CLEARANCE
    } else if (sameDirection) {
      laneX =
        Math.max(fromPosition.x + nodeWidth(fromState), toPosition.x + nodeWidth(toState)) +
        PORT_GAP +
        ROUTE_CLEARANCE
    }
    return compactPoints([
      source,
      sourceExit,
      laneSource,
      { x: laneX, y: laneSource.y },
      { x: laneX, y: laneTarget.y },
      laneTarget,
      targetExit,
      target
    ])
  }
  if (!horizontal(fromDirection) && !horizontal(toDirection)) {
    const sameDirection = fromDirection === toDirection
    let laneY = (laneSource.y + laneTarget.y) / 2
    if (sameDirection && fromDirection === 'top') {
      laneY = Math.min(fromPosition.y, toPosition.y) - PORT_GAP - ROUTE_CLEARANCE
    } else if (sameDirection) {
      laneY =
        Math.max(fromPosition.y, toPosition.y) + NODE_HEIGHT + PORT_GAP + ROUTE_CLEARANCE
    }
    return compactPoints([
      source,
      sourceExit,
      laneSource,
      { x: laneSource.x, y: laneY },
      { x: laneTarget.x, y: laneY },
      laneTarget,
      targetExit,
      target
    ])
  }
  return compactPoints([
    source,
    sourceExit,
    laneSource,
    { x: laneTarget.x, y: laneSource.y },
    laneTarget,
    targetExit,
    target
  ])
}

const transitionLines = computed<GraphLine[]>(() => {
  const groups: Array<{ pairKey: string; transitions: GraphTransition[] }> = []
  const consumed = new Set<string>()

  transitions.forEach((transition) => {
    const key = transitionKey(transition)
    if (consumed.has(key)) return

    const reverse = transitions.find(
      (candidate) =>
        candidate.fromStateId === transition.toStateId &&
        candidate.toStateId === transition.fromStateId &&
        candidate.event === transition.event &&
        candidate.fromPort === transition.toPort &&
        candidate.toPort === transition.fromPort &&
        !consumed.has(transitionKey(candidate))
    )
    const lineTransitions = reverse ? [transition, reverse] : [transition]
    lineTransitions.forEach((item) => consumed.add(transitionKey(item)))
    groups.push({
      pairKey: pairKey(transition.fromStateId, transition.toStateId),
      transitions: lineTransitions
    })
  })

  const pairLineCounts = new Map<string, number>()
  groups.forEach((group) => {
    pairLineCounts.set(group.pairKey, (pairLineCounts.get(group.pairKey) ?? 0) + 1)
  })
  const pairLineIndexes = new Map<string, number>()

  return groups.flatMap(({ pairKey: key, transitions: group }) => {
    const selection =
      group.find((transition) => transitionKey(transition) === selectedTransitionKey) ?? group[0]
    const from = states.find((state) => state.id === selection.fromStateId)
    const to = states.find((state) => state.id === selection.toStateId)
    if (!from || !to) return []
    const lineIndex = pairLineIndexes.get(key) ?? 0
    pairLineIndexes.set(key, lineIndex + 1)
    const lineCount = pairLineCounts.get(key) ?? 1
    const laneOffset = lineCount > 1 ? (lineIndex - (lineCount - 1) / 2) * 18 : 0
    const points = routeBetween(selection, from, to, laneOffset)
    if (!points) return []
    return [
      {
        transitions: group,
        labelTransitions: [selection],
        selection,
        key: group.map(transitionKey).sort().join('|'),
        path: roundedPath(points),
        segments: points.slice(1).map((point, index) => ({ from: points[index], to: point })),
        label: lineLabelPoint(points, lineIndex),
        bidirectional: group.length === 2,
        selected: group.some((transition) => transitionKey(transition) === selectedTransitionKey)
      }
    ]
  })
})

const connectionPreview = computed(() => {
  if (!connecting.value) return ''
  const source = states.find((state) => state.id === connecting.value?.fromStateId)
  const position = source ? positions[source.id] : null
  if (!source || !position) return ''
  const start = portPoint(source, position, connecting.value.direction)
  const exit = movePoint(start, connecting.value.direction, PORT_GAP)
  const end = connecting.value.point
  return roundedPath(compactPoints([start, exit, { x: end.x, y: exit.y }, end]))
})

function selectLine(line: GraphLine) {
  if (line.transitions.every((transition) => transition.selectable === false)) return
  emit('select-transition', line.selection)
}

function startNodeDrag(event: PointerEvent, stateId: string) {
  if (event.button !== 0 || zPressed.value || connecting.value || pinchState) return
  const point = graphPoint(event)
  const position = positions[stateId]
  if (!point || !position) return
  event.preventDefault()
  event.stopPropagation()
  dragState = { stateId, offset: { x: point.x - position.x, y: point.y - position.y } }
}

function startConnect(
  event: PointerEvent,
  stateId: string,
  direction: DevicePrototypePortDirection
) {
  if (interaction?.mode !== 'custom' || event.button !== 0 || zPressed.value || pinchState) return
  const point = graphPoint(event)
  if (!point) return
  event.preventDefault()
  event.stopPropagation()
  dragState = null
  connecting.value = { fromStateId: stateId, direction, point }
}

function portAtPoint(
  clientX: number,
  clientY: number
): { stateId: string; direction: DevicePrototypePortDirection } | null {
  const element = document.elementFromPoint(clientX, clientY)
  const port = element?.closest<HTMLElement>('[data-device-prototype-port]')
  const stateId = port?.dataset.devicePrototypePortStateId
  const direction = port?.dataset.devicePrototypePortDirection as
    | DevicePrototypePortDirection
    | undefined
  if (!stateId || !direction) return null
  return { stateId, direction }
}

function isInputTarget(target: EventTarget | null): boolean {
  return (
    target instanceof HTMLElement &&
    Boolean(target.closest('input, textarea, [contenteditable="true"]'))
  )
}

function orientation(a: Point, b: Point, c: Point): number {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)
}

function within(value: number, first: number, second: number): boolean {
  return value >= Math.min(first, second) - 0.01 && value <= Math.max(first, second) + 0.01
}

function segmentsIntersect(first: Segment, second: Segment): boolean {
  const a = orientation(first.from, first.to, second.from)
  const b = orientation(first.from, first.to, second.to)
  const c = orientation(second.from, second.to, first.from)
  const d = orientation(second.from, second.to, first.to)
  if (
    a === 0 &&
    within(second.from.x, first.from.x, first.to.x) &&
    within(second.from.y, first.from.y, first.to.y)
  )
    return true
  if (
    b === 0 &&
    within(second.to.x, first.from.x, first.to.x) &&
    within(second.to.y, first.from.y, first.to.y)
  )
    return true
  if (
    c === 0 &&
    within(first.from.x, second.from.x, second.to.x) &&
    within(first.from.y, second.from.y, second.to.y)
  )
    return true
  if (
    d === 0 &&
    within(first.to.x, second.from.x, second.to.x) &&
    within(first.to.y, second.from.y, second.to.y)
  )
    return true
  return a > 0 !== b > 0 && c > 0 !== d > 0
}

function cutTransitions(segment: Segment) {
  const cut = cutting.value
  if (!cut) return
  transitionLines.value.forEach((line) => {
    if (!line.segments.some((lineSegment) => segmentsIntersect(segment, lineSegment))) return
    line.transitions.forEach((transition) => {
      const key = transitionKey(transition)
      if (cut.removed.has(key) || transition.selectable === false) return
      cut.removed.add(key)
      emit('remove-transition', transition)
    })
  })
}

function onViewportPointerDown(event: PointerEvent) {
  if (event.pointerType === 'touch') {
    trackTouchPointer(event)
    return
  }
  if (event.button === 1) {
    event.preventDefault()
    panState = { point: { x: event.clientX, y: event.clientY }, pan: { ...pan } }
    graphViewport.value?.setPointerCapture(event.pointerId)
    return
  }
  if (event.button !== 0 || !zPressed.value) return
  const point = graphPoint(event)
  if (!point) return
  event.preventDefault()
  cutting.value = { points: [point], removed: new Set() }
}

function trackTouchPointer(event: PointerEvent) {
  if (event.pointerType !== 'touch') return
  const point = viewportPoint(event.clientX, event.clientY)
  if (!point) return
  touchPointers.set(event.pointerId, point)
  if (touchPointers.size === 2) beginPinch()
}

function beginPinch() {
  const [first, second] = [...touchPointers.values()]
  if (!first || !second) return
  dragState = null
  connecting.value = null
  const center = { x: (first.x + second.x) / 2, y: (first.y + second.y) / 2 }
  pinchState = {
    distance: Math.hypot(second.x - first.x, second.y - first.y),
    center,
    zoom: zoom.value,
    pan: { ...pan }
  }
}

function movePinch() {
  if (!pinchState || touchPointers.size < 2) return
  const [first, second] = [...touchPointers.values()]
  if (!first || !second) return
  const center = { x: (first.x + second.x) / 2, y: (first.y + second.y) / 2 }
  const distance = Math.hypot(second.x - first.x, second.y - first.y)
  if (!pinchState.distance) return
  const nextZoom = clampZoom(pinchState.zoom * (distance / pinchState.distance))
  const ratio = nextZoom / pinchState.zoom
  pan.x =
    pinchState.center.x -
    (pinchState.center.x - pinchState.pan.x) * ratio +
    center.x -
    pinchState.center.x
  pan.y =
    pinchState.center.y -
    (pinchState.center.y - pinchState.pan.y) * ratio +
    center.y -
    pinchState.center.y
  zoom.value = nextZoom
}

function onGlobalPointerMove(event: PointerEvent) {
  if (event.pointerType === 'touch' && touchPointers.has(event.pointerId)) {
    const point = viewportPoint(event.clientX, event.clientY)
    if (point) touchPointers.set(event.pointerId, point)
    movePinch()
    return
  }
  if (panState) {
    pan.x = panState.pan.x + event.clientX - panState.point.x
    pan.y = panState.pan.y + event.clientY - panState.point.y
    return
  }
  if (dragState) {
    const point = graphPoint(event)
    if (!point) return
    positions[dragState.stateId] = {
      x: point.x - dragState.offset.x,
      y: point.y - dragState.offset.y
    }
    return
  }
  if (connecting.value) {
    const point = graphPoint(event)
    if (point) connecting.value = { ...connecting.value, point }
    return
  }
  if (cutting.value) {
    const point = graphPoint(event)
    const last = cutting.value.points.at(-1)
    if (!point || !last) return
    cutting.value.points.push(point)
    cutTransitions({ from: last, to: point })
  }
}

function finishPointerInteraction(event: PointerEvent) {
  if (event.pointerType === 'touch') {
    touchPointers.delete(event.pointerId)
    if (touchPointers.size < 2) pinchState = null
    if (!touchPointers.size) dragState = null
    return
  }
  if (connecting.value) {
    const sourceId = connecting.value.fromStateId
    const fromPort = connecting.value.direction
    const targetPort = portAtPoint(event.clientX, event.clientY)
    connecting.value = null
    if (targetPort && targetPort.stateId !== sourceId) {
      emit('connect', sourceId, targetPort.stateId, fromPort, targetPort.direction)
    }
  }
  dragState = null
  panState = null
  cutting.value = null
}

function onWheel(event: WheelEvent) {
  const viewport = graphViewport.value
  if (!viewport || !(event.target instanceof Node) || !viewport.contains(event.target)) return
  event.preventDefault()
  if (event.ctrlKey || event.metaKey) {
    const point = viewportPoint(event.clientX, event.clientY)
    if (point) zoomAround(zoom.value * Math.exp(-event.deltaY * 0.002), point)
    return
  }
  const horizontal = event.shiftKey && Math.abs(event.deltaY) > Math.abs(event.deltaX)
  pan.x -= horizontal ? event.deltaY : event.deltaX
  pan.y -= horizontal ? 0 : event.deltaY
}

function selectStateWithKeyboard(event: KeyboardEvent, stateId: string) {
  if (event.key !== 'Enter' && event.key !== ' ') return
  event.preventDefault()
  emit('select-state', stateId)
}

function handleKeyDown(event: KeyboardEvent) {
  if (event.key.toLowerCase() === 'z' && !isInputTarget(event.target)) zPressed.value = true
}

function handleKeyUp(event: KeyboardEvent) {
  if (event.key.toLowerCase() === 'z') zPressed.value = false
}

function cancelInteractions() {
  dragState = null
  panState = null
  connecting.value = null
  cutting.value = null
  pinchState = null
  touchPointers.clear()
  zPressed.value = false
}

watch(
  () => interaction?.id,
  () => inferredPortPairs.clear(),
  { immediate: true }
)

watch(
  stateSignature,
  () => {
    syncPositions()
    if (!hasInitialized && states.length) {
      hasInitialized = true
      nextTick(() => {
        zoom.value = 1
        pan.x = 40
        pan.y = 44
      })
    }
  },
  { immediate: true }
)
useEventListener(window, 'pointermove', onGlobalPointerMove)
useEventListener(window, 'pointerup', finishPointerInteraction)
useEventListener(window, 'pointercancel', finishPointerInteraction)
useEventListener(window, 'keydown', handleKeyDown)
useEventListener(window, 'keyup', handleKeyUp)
useEventListener(window, 'blur', cancelInteractions)
useEventListener(window, 'wheel', onWheel, { capture: true, passive: false })

onUnmounted(cancelInteractions)
</script>

<template>
  <div
    class="flex h-full min-h-0 flex-col overflow-hidden rounded-panel border border-border bg-panel-field"
  >
    <div
      ref="graphViewport"
      class="relative min-h-0 flex-1 touch-none overflow-hidden bg-[radial-gradient(circle_at_1px_1px,color-mix(in_srgb,var(--color-border)_35%,transparent)_1px,transparent_0)]"
      :class="zPressed ? 'cursor-crosshair' : 'cursor-default'"
      :style="gridStyle"
      @contextmenu.prevent
      @pointerdown.capture="onViewportPointerDown"
    >
      <div
        class="absolute left-0 top-0 size-px overflow-visible"
        :style="{ transform: worldTransform, transformOrigin: 'top left' }"
      >
        <svg
          class="pointer-events-none absolute left-0 top-0 overflow-visible"
          width="1"
          height="1"
          aria-hidden="true"
        >
          <defs>
            <marker
              id="device-prototype-arrow"
              markerWidth="7"
              markerHeight="7"
              refX="6"
              refY="3.5"
              orient="auto-start-reverse"
            >
              <path d="M0,0 L7,3.5 L0,7 z" fill="currentColor" />
            </marker>
          </defs>
          <g
            v-for="line in transitionLines"
            :key="line.key"
            class="pointer-events-auto cursor-pointer"
            @pointerdown.stop
            @click.stop="selectLine(line)"
          >
            <path :d="line.path" fill="none" stroke="transparent" stroke-width="14" />
            <path
              :d="line.path"
              fill="none"
              stroke="currentColor"
              stroke-width="1.75"
              :class="line.selected ? 'text-accent' : 'text-border-strong'"
              :marker-start="line.bidirectional ? 'url(#device-prototype-arrow)' : undefined"
              marker-end="url(#device-prototype-arrow)"
              class="pointer-events-none"
            />
          </g>
          <path
            v-if="connectionPreview"
            :d="connectionPreview"
            fill="none"
            stroke="currentColor"
            stroke-width="1.75"
            stroke-dasharray="4 4"
            class="text-accent"
            marker-end="url(#device-prototype-arrow)"
          />
        </svg>

        <div
          v-for="line in transitionLines"
          :key="`${line.key}:labels`"
          class="absolute z-20 flex -translate-x-1/2 -translate-y-1/2 flex-col items-center gap-1"
          :style="{ left: `${line.label.x}px`, top: `${line.label.y}px` }"
        >
          <button
            v-for="transition in line.labelTransitions"
            :key="transitionKey(transition)"
            type="button"
            class="h-5 max-w-32 truncate rounded border bg-panel px-1.5 text-[8px] font-medium shadow-sm"
            :class="
              transitionKey(transition) === selectedTransitionKey
                ? 'border-accent text-accent'
                : 'border-border text-muted hover:border-border-strong hover:text-surface'
            "
            :disabled="transition.selectable === false"
            :aria-label="`选择跳转：${transitionLabel(transition)}`"
            @pointerdown.stop
            @click.stop="emit('select-transition', transition)"
          >
            {{ transitionLabel(transition) }}
          </button>
        </div>

        <div
          v-for="state in states"
          :key="state.id"
          role="button"
          tabindex="0"
          :data-device-prototype-state-id="state.id"
          class="group absolute z-10 flex h-[34px] select-none items-center rounded-md border bg-panel px-2.5 text-left shadow-sm outline-none transition-colors cursor-grab active:cursor-grabbing"
          :class="
            state.id === selectedStateId
              ? 'border-accent ring-1 ring-accent/30'
              : 'border-border hover:border-border-strong'
          "
          :style="{
            left: `${positions[state.id]?.x ?? 0}px`,
            top: `${positions[state.id]?.y ?? 0}px`,
            width: `${nodeWidth(state)}px`
          }"
          :aria-label="`选择并拖动 ${state.name}`"
          @pointerdown="startNodeDrag($event, state.id)"
          @click="emit('select-state', state.id)"
          @keydown="selectStateWithKeyboard($event, state.id)"
        >
          <span
            v-if="state.id === initialStateId"
            class="mr-1.5 size-1.5 shrink-0 rounded-full bg-success"
            aria-label="初始状态"
          />
          <span class="min-w-0 flex-1 truncate text-[10px] font-medium text-surface">{{
            state.name
          }}</span>
          <template v-if="interaction?.mode === 'custom'">
            <button
              v-for="direction in portDirections"
              :key="direction"
              type="button"
              class="absolute z-20 flex size-4 items-center justify-center rounded-full border border-accent bg-panel text-accent opacity-0 transition-opacity group-hover:opacity-100 focus:opacity-100"
              :class="{
                '-top-2 left-1/2 -translate-x-1/2': direction === 'top',
                '-right-2 top-1/2 -translate-y-1/2': direction === 'right',
                '-bottom-2 left-1/2 -translate-x-1/2': direction === 'bottom',
                '-left-2 top-1/2 -translate-y-1/2': direction === 'left'
              }"
              :aria-label="`从 ${state.name} 创建连接`"
              data-device-prototype-port
              :data-device-prototype-port-state-id="state.id"
              :data-device-prototype-port-direction="direction"
              @pointerdown="startConnect($event, state.id, direction)"
              @click.stop
            >
              <icon-lucide-plus class="size-2.5" />
            </button>
          </template>
        </div>
      </div>
    </div>
  </div>
</template>
