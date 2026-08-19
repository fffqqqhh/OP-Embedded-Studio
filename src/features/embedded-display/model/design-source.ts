/** A design source item exposed to embedded-device workflows. */
export interface EmbeddedDesignSourceItem {
  id: string
  name: string
  sourceKind: 'frame' | 'image'
  width: number
  height: number
}

export interface EmbeddedDesignSourceSummary {
  layerCount: number
  textSamples: string[]
}

/**
 * The narrow design boundary required by AI, interaction, and device workflows.
 * Implementations may come from OpenPencil, a test fixture, or another editor.
 */
export interface EmbeddedDesignSource {
  getDocumentName(): string
  getRevision(): number
  getSelectedSources(): EmbeddedDesignSourceItem[]
  getSelectionError?(): string | null
  getPageSources(): EmbeddedDesignSourceItem[]
  getSource(id: string): EmbeddedDesignSourceItem | null
  getSourceSummary(id: string): EmbeddedDesignSourceSummary | null
  renderSourcePng(id: string): Promise<Uint8Array>
}
