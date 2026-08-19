const PROFILE_NAME_PATTERNS: Array<[RegExp, string]> = [
  [/stopwatch/i, 'StopWatch'],
  [/cores3/i, 'CoreS3'],
  [/waveshare/i, 'Waveshare']
]

function compactProfileName(profileName: string): string {
  for (const [pattern, name] of PROFILE_NAME_PATTERNS) {
    if (pattern.test(profileName)) return name
  }

  const compact = profileName
    .replace(/\b\d+\s*x\s*\d+\b/gi, '')
    .replace(/\b(?:ST7789|ST7735S|GC9D01N|CO5300|ILI9342C)\b/gi, '')
    .replace(/圆形|方屏|圆屏|AMOLED/gi, '')
    .replace(/\s+/g, ' ')
    .trim()

  return compact.slice(0, 18) || 'Frame'
}

export function createPresetFrameName(
  profileName: string,
  existingNames: Iterable<string>
): string {
  const baseName = compactProfileName(profileName)
  const names = new Set(existingNames)
  let index = 1
  while (names.has(`${baseName} ${index}`)) index += 1
  return `${baseName} ${index}`
}
