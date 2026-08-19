import type {
  EmbeddedDesignSource,
  EmbeddedDesignSourceItem,
  EmbeddedDesignSourceSummary
} from '@/features/embedded-display'

export interface FakeEmbeddedDesignSourceItem extends EmbeddedDesignSourceItem {
  png?: Uint8Array
  summary?: EmbeddedDesignSourceSummary
}

export class FakeEmbeddedDesignSource implements EmbeddedDesignSource {
  private revision = 1
  private selectedIds: string[] = []
  private selectionError: string | null = null
  private readonly items = new Map<string, EmbeddedDesignSourceItem>()
  private readonly pngById = new Map<string, Uint8Array>()
  private readonly summaries = new Map<string, EmbeddedDesignSourceSummary>()

  constructor(
    items: FakeEmbeddedDesignSourceItem[],
    private readonly documentName = 'Fake embedded design'
  ) {
    for (const { png, summary, ...item } of items) {
      this.items.set(item.id, { ...item })
      this.pngById.set(item.id, Uint8Array.from(png ?? [item.id.length]))
      this.summaries.set(item.id, summary ?? { layerCount: 0, textSamples: [] })
    }
  }

  getDocumentName(): string {
    return this.documentName
  }

  getRevision(): number {
    return this.revision
  }

  getSelectedSources(): EmbeddedDesignSourceItem[] {
    return this.selectedIds.flatMap((id) => {
      const item = this.items.get(id)
      return item ? [{ ...item }] : []
    })
  }

  getSelectionError(): string | null {
    return this.selectionError
  }

  getPageSources(): EmbeddedDesignSourceItem[] {
    return [...this.items.values()].map((item) => ({ ...item }))
  }

  getSource(id: string): EmbeddedDesignSourceItem | null {
    const item = this.items.get(id)
    return item ? { ...item } : null
  }

  getSourceSummary(id: string): EmbeddedDesignSourceSummary | null {
    const summary = this.summaries.get(id)
    return summary
      ? { layerCount: summary.layerCount, textSamples: [...summary.textSamples] }
      : null
  }

  async renderSourcePng(id: string): Promise<Uint8Array> {
    const png = this.pngById.get(id)
    if (!png || !this.items.has(id)) throw new Error(`Missing fake source: ${id}`)
    return Uint8Array.from(png)
  }

  select(ids: string[]): void {
    this.selectedIds = [...ids]
    this.selectionError = null
  }

  setSelectionError(error: string | null): void {
    this.selectedIds = []
    this.selectionError = error
  }

  advanceRevision(): number {
    this.revision += 1
    return this.revision
  }

  removeSource(id: string): void {
    this.items.delete(id)
    this.pngById.delete(id)
    this.summaries.delete(id)
    this.selectedIds = this.selectedIds.filter((selectedId) => selectedId !== id)
    this.advanceRevision()
  }
}
