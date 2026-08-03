//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Static QML scanner producing the candidate element catalog for the flow
 * editor and the catalog page. It walks every `*.qml` file under the scanned
 * root, tracks the enclosing QML component for each `objectName` binding, and
 * classifies the binding:
 *
 * - static literal (`objectName: "libraryNavButton"`) — one exact candidate;
 * - ternary / conditional (`index === 0 ? "firstCard" : "card_" + index`) —
 *   every string literal becomes a candidate, concatenations additionally yield
 *   a regex pattern so runtime names such as `thumbnailGridView_card_12` match;
 * - unresolvable dynamic (`String(entry.itemObjectName || "")`) — kept in the
 *   catalog for visibility but excluded from staleness diffing.
 *
 * The catalog is advisory: the runner always resolves targets against the live
 * `QQuickItem` tree via `snapshot`, and the catalog page marks entries missing
 * at runtime as stale (see {@link ./catalog-staleness.js}).
 */

import { readdir, readFile } from "node:fs/promises";
import { join, relative, sep } from "node:path";

/** Interaction kinds an element is likely to support, inferred from its component type. */
export type OpKind = "click" | "doubleClick" | "rightClick" | "drag" | "typeText" | "key" | "wait";

/** One scanned `objectName` binding. */
export interface CatalogEntry {
  /** Resolved literal name for static bindings; null for dynamic ones. */
  readonly objectName: string | null;
  /** Every non-empty string literal in the binding expression. */
  readonly candidates: readonly string[];
  /** Regex source for concatenated bindings (`"card_" + index` -> `^card_.*$`); null otherwise. */
  readonly pattern: string | null;
  /** True when the binding expression is not a single string literal. */
  readonly dynamic: boolean;
  /** Raw binding expression, whitespace-collapsed onto one line. */
  readonly expression: string;
  /** Enclosing QML component type name (e.g. `IconActionButton`). */
  readonly component: string;
  /** Inferred interaction kinds; `wait` (property assertion) applies to every entry. */
  readonly opKinds: readonly OpKind[];
  /** Path of the QML file relative to the scanned root, POSIX separators. */
  readonly source: string;
  /** 1-based line of the `objectName` key. */
  readonly line: number;
}

/** Result of scanning a QML directory tree. */
export interface QmlCatalog {
  readonly root: string;
  readonly generatedAt: number;
  readonly filesScanned: number;
  readonly entries: readonly CatalogEntry[];
}

interface ComponentFrame {
  readonly type: string;
  readonly depth: number;
}

const OBJECT_NAME_KEY = /\bobjectName\s*:\s*(.*)$/;
const OBJECT_HEADER = /(?:^|[^A-Za-z0-9_$.])([A-Z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)?)\s*\{/g;
const STRING_LITERAL = /"((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)'/g;

/**
 * Component-type inference rules, evaluated in order; the first match wins.
 * More specific names (sliders, text inputs) precede generic ones (buttons,
 * views) so e.g. `SearchComboBox` is treated as text input rather than a plain
 * combo box.
 */
const OP_KIND_RULES: ReadonlyArray<{ readonly pattern: RegExp; readonly kinds: readonly OpKind[] }> = [
  { pattern: /slider/i, kinds: ["click", "drag"] },
  { pattern: /searchcombo|textfield|textarea|textinput|textedit|field/i, kinds: ["click", "typeText", "key"] },
  { pattern: /button|menuitem|toggle|checkbox|radiobutton|switch/i, kinds: ["click"] },
  { pattern: /combo/i, kinds: ["click"] },
  { pattern: /mousearea|tap/i, kinds: ["click", "doubleClick", "rightClick"] },
  { pattern: /gridview|listview|tableview|treeview|pathview/i, kinds: ["click", "doubleClick"] },
  { pattern: /menu/i, kinds: ["click"] },
];

/** Infers interaction kinds from the enclosing component type name. */
export function inferOpKinds(component: string): readonly OpKind[] {
  const rule = OP_KIND_RULES.find((candidate) => candidate.pattern.test(component));
  const interaction = rule?.kinds ?? [];
  // Property assertions compile to probe `wait` calls and apply to every element.
  return [...interaction, "wait"];
}

/**
 * Scans one QML source text for `objectName` bindings.
 *
 * @param sourceName value recorded in each entry's `source` field.
 */
export function scanQmlSource(text: string, sourceName: string): CatalogEntry[] {
  const entries: CatalogEntry[] = [];
  const stack: ComponentFrame[] = [];
  let depth = 0;
  let inBlockComment = false;
  const lines = text.split("\n");

  for (let index = 0; index < lines.length; index++) {
    const cleaned = stripComments(lines[index]!, (state) => {
      inBlockComment = state;
    }, inBlockComment);

    for (const match of cleaned.code.matchAll(OBJECT_HEADER)) {
      stack.push({ type: match[1]!, depth: depth + 1 });
    }

    const keyMatch = OBJECT_NAME_KEY.exec(cleaned.code);
    if (keyMatch !== null) {
      const expression = collectBindingExpression(lines, index, keyMatch[1]!);
      const component = stack.at(-1)?.type ?? "(file scope)";
      entries.push(classifyBinding(expression, component, sourceName, index + 1));
    }

    depth += cleaned.braceDelta;
    while (stack.length > 0 && stack.at(-1)!.depth > depth) {
      stack.pop();
    }
  }
  return entries;
}

/**
 * Recursively scans a directory for `*.qml` files and returns the catalog.
 * Paths are sorted for deterministic output.
 */
export async function scanQmlDirectory(rootDir: string): Promise<QmlCatalog> {
  const files = await collectQmlFiles(rootDir);
  const entries: CatalogEntry[] = [];
  for (const file of files) {
    const text = await readFile(file, "utf8");
    const sourceName = relative(rootDir, file).split(sep).join("/");
    entries.push(...scanQmlSource(text, sourceName));
  }
  return { root: rootDir, generatedAt: Date.now(), filesScanned: files.length, entries };
}

async function collectQmlFiles(rootDir: string): Promise<string[]> {
  const result: string[] = [];
  const walk = async (dir: string): Promise<void> => {
    const listing = await readdir(dir, { withFileTypes: true });
    for (const item of listing) {
      const full = join(dir, item.name);
      if (item.isDirectory()) {
        await walk(full);
      } else if (item.isFile() && item.name.endsWith(".qml")) {
        result.push(full);
      }
    }
  };
  await walk(rootDir);
  result.sort();
  return result;
}

interface CleanedLine {
  readonly code: string;
  readonly braceDelta: number;
}

/**
 * Strips `//` and block comments while respecting string literals, and reports
 * the net `{`/`}` delta of the remaining code. Block-comment state is carried
 * across lines via `inBlockComment` / the state callback.
 */
function stripComments(
  line: string,
  setBlockComment: (state: boolean) => void,
  inBlockComment: boolean,
): CleanedLine {
  let code = "";
  let braceDelta = 0;
  let quote: string | null = null;
  for (let index = 0; index < line.length; index++) {
    const char = line[index]!;
    const next = line[index + 1];
    if (inBlockComment) {
      if (char === "*" && next === "/") {
        inBlockComment = false;
        setBlockComment(false);
        index++;
      }
      continue;
    }
    if (quote !== null) {
      code += char;
      if (char === "\\") {
        code += next ?? "";
        index++;
      } else if (char === quote) {
        quote = null;
      }
      continue;
    }
    if (char === "/" && next === "/") break;
    if (char === "/" && next === "*") {
      inBlockComment = true;
      setBlockComment(true);
      index++;
      continue;
    }
    if (char === '"' || char === "'") {
      quote = char;
      code += char;
      continue;
    }
    if (char === "{") braceDelta++;
    if (char === "}") braceDelta--;
    code += char;
  }
  return { code, braceDelta };
}

/** Tokens that indicate the binding continues on the next line. */
const CONTINUATION_SUFFIX = /(?:\?|:|\+|\|\||&&|\||,|\.|\(|\[)\s*$/;
const CONTINUATION_PREFIX = /^\s*(?::|\?|\+|\.)/;

/**
 * Collects a possibly multi-line binding expression starting at `fromIndex`,
 * continuing while parentheses are open or the expression obviously continues
 * (ternary `:`/`?`, concatenation `+`, member `.`).
 */
function collectBindingExpression(lines: readonly string[], fromIndex: number, firstChunk: string): string {
  const chunks = [firstChunk.trim()];
  let parenDepth = countParenDelta(firstChunk);
  let cursor = fromIndex;
  while (cursor + 1 < lines.length) {
    const current = chunks.at(-1)!;
    const nextLine = lines[cursor + 1]!;
    if (parenDepth <= 0 && !CONTINUATION_SUFFIX.test(current) && !CONTINUATION_PREFIX.test(nextLine)) {
      break;
    }
    if (nextLine.trim().length === 0) break;
    chunks.push(nextLine.trim());
    cursor++;
    parenDepth += countParenDelta(nextLine);
    if (parenDepth <= 0 && !CONTINUATION_SUFFIX.test(nextLine.trim())) break;
  }
  return chunks.join(" ").replace(/\s+/g, " ").trim();
}

function countParenDelta(text: string): number {
  let delta = 0;
  let quote: string | null = null;
  for (let index = 0; index < text.length; index++) {
    const char = text[index]!;
    if (quote !== null) {
      if (char === "\\") index++;
      else if (char === quote) quote = null;
      continue;
    }
    if (char === '"' || char === "'") {
      quote = char;
    } else if (char === "(" || char === "[") {
      delta++;
    } else if (char === ")" || char === "]") {
      delta--;
    }
  }
  return delta;
}

function extractStringLiterals(expression: string): string[] {
  const literals: string[] = [];
  for (const match of expression.matchAll(STRING_LITERAL)) {
    const value = match[1] ?? match[2] ?? "";
    if (value.length > 0) literals.push(value);
  }
  return literals;
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

const LITERAL_MARKER = /"((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)'/g;

/**
 * Derives regex sources for concatenated bindings. String literals are first
 * masked out, the expression is split at ternary `?`/`:` boundaries, and each
 * branch containing a `+` concatenation with literals contributes one pattern
 * built from its literals in order (e.g. `("card_" + index)` -> `card_.*`).
 * Pure literal branches stay exact candidates and contribute no pattern.
 */
function deriveBranchPatterns(expression: string): string[] {
  const literals: string[] = [];
  const masked = expression.replace(LITERAL_MARKER, (_match, doubleQuoted, singleQuoted) => {
    literals.push((doubleQuoted ?? singleQuoted ?? "") as string);
    return ` ${literals.length - 1} `;
  });

  const patterns: string[] = [];
  for (const branch of masked.split(/[?:]/)) {
    if (!branch.includes("+")) continue;
    // Rebuild the chain segment by segment: literal markers become their
    // escaped text, every dynamic segment becomes `.*`.
    const segments: string[] = [];
    for (const part of branch.split("+")) {
      const marker = / (\d+) /.exec(part);
      if (marker !== null) {
        const literal = literals[Number(marker[1])] ?? "";
        if (literal.length > 0) segments.push(escapeRegExp(literal));
      } else if (part.trim().length > 0) {
        segments.push(".*");
      }
    }
    if (segments.length === 0) continue;
    patterns.push(segments.join("").replace(/(?:\.\*){2,}/g, ".*"));
  }
  return patterns;
}

function classifyBinding(
  expression: string,
  component: string,
  source: string,
  line: number,
): CatalogEntry {
  const candidates = extractStringLiterals(expression);
  const trimmed = expression.trim();
  const staticMatch = /^(?:"((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)')$/.exec(trimmed);
  const isStatic = staticMatch !== null;
  const objectName = isStatic ? (staticMatch[1] ?? staticMatch[2] ?? "") : null;

  let pattern: string | null = null;
  if (!isStatic) {
    const branchPatterns = deriveBranchPatterns(trimmed);
    if (branchPatterns.length > 0) {
      pattern = `^(?:${branchPatterns.join("|")})$`;
    }
  }

  return {
    objectName,
    candidates,
    pattern,
    dynamic: !isStatic,
    expression: trimmed,
    component,
    opKinds: inferOpKinds(component),
    source,
    line,
  };
}
