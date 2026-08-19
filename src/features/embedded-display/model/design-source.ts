/** A design source item exposed to embedded-device workflows. */
export interface EmbeddedDesignSourceItem {
  id: string
  name: string
  sourceKind: 'frame' | 'image'
  width: number
  height: number
}

/**
 * The narrow design boundary required by AI, interaction, and device workflows.
 * Implementations may come from OpenPencil, a test fixture, or another editor.
 */
export interface EmbeddedDesignSource {
  getRevision(): number
  getSelectedSources(): EmbeddedDesignSourceItem[]
  getSelectionError?(): string | null
  getPageSources(): EmbeddedDesignSourceItem[]
  getSource(id: string): EmbeddedDesignSourceItem | null
  renderSourcePng(id: string): Promise<Uint8Array>
}
