/**
 * Conversation Store — persists chat conversations in localStorage.
 *
 * Data model per conversation:
 * {
 *   id: string,          // UUID (same as sessionId)
 *   title: string,       // derived from first user message
 *   createdAt: number,   // Date.now()
 *   updatedAt: number,   // Date.now()
 *   messages: [          // serialisable message objects
 *     { role, text, status?, history?, commands?, ts? }
 *   ]
 * }
 */

const STORAGE_KEY = "mpx_chat_conversations";

// ── UUID (matches Chat.svelte) ─────────────────────────────────
function generateUUID() {
  if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
    return crypto.randomUUID();
  }
  return "10000000-1000-4000-8000-100000000000".replace(/[018]/g, (c) =>
    (c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (c / 4)))).toString(16)
  );
}

// ── Internal helpers ───────────────────────────────────────────

function readAll() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw ? JSON.parse(raw) : {};
  } catch {
    return {};
  }
}

function writeAll(data) {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(data));
  } catch (e) {
    console.warn("conversationStore: unable to write to localStorage", e);
  }
}

/** Generate a short title from message text, falling back to a placeholder. */
function inferTitle(messages) {
  const firstUser = messages.find((m) => m.role === "user");
  if (firstUser) {
    const raw = firstUser.text.trim();
    // Take the first line or first 48 chars, whichever is shorter
    const newlineIdx = raw.indexOf("\n");
    const short = newlineIdx === -1 ? raw : raw.slice(0, newlineIdx);
    return short.length > 48 ? short.slice(0, 45) + "…" : short;
  }
  return "New Conversation";
}

// ── Public API ─────────────────────────────────────────────────

/**
 * Return every saved conversation, newest first.
 */
export function listConversations() {
  const all = readAll();
  return Object.values(all).sort((a, b) => b.updatedAt - a.updatedAt);
}

/**
 * Get a single conversation by id.
 */
export function getConversation(id) {
  const all = readAll();
  return all[id] || null;
}

/**
 * Persist (create or update) a conversation.
 */
export function saveConversation(id, { messages, title }) {
  const all = readAll();
  const existing = all[id];
  const now = Date.now();

  all[id] = {
    id,
    title: title ?? (messages ? inferTitle(messages) : existing?.title ?? "New Conversation"),
    createdAt: existing?.createdAt ?? now,
    updatedAt: now,
    messages: messages ?? existing?.messages ?? [],
  };

  writeAll(all);
  return all[id];
}

/**
 * Remove a conversation by id.
 */
export function deleteConversation(id) {
  const all = readAll();
  delete all[id];
  writeAll(all);
}

/**
 * Delete all conversations.
 */
export function clearAllConversations() {
  writeAll({});
}

/**
 * Return a JSON string of all conversations (for export).
 */
export function exportConversations() {
  return JSON.stringify(readAll(), null, 2);
}

/**
 * Merge imported conversations into the store.
 * Returns the number of conversations imported.
 * Optionally pass `onConflict: "skip" | "overwrite"` (default: "overwrite").
 */
export function importConversations(jsonStr, { onConflict = "overwrite" } = {}) {
  let imported;
  try {
    imported = JSON.parse(jsonStr);
  } catch {
    throw new Error("Invalid JSON");
  }
  if (typeof imported !== "object" || imported === null || Array.isArray(imported)) {
    throw new Error("Expected an object mapping session IDs to conversations");
  }

  const all = readAll();
  let count = 0;

  for (const [id, convo] of Object.entries(imported)) {
    if (!convo || typeof convo !== "object") continue;
    if (!convo.messages || !Array.isArray(convo.messages)) continue;

    if (onConflict === "skip" && all[id]) continue;

    all[id] = {
      id,
      title: convo.title || inferTitle(convo.messages),
      createdAt: convo.createdAt ?? Date.now(),
      updatedAt: convo.updatedAt ?? Date.now(),
      messages: convo.messages,
    };
    count++;
  }

  writeAll(all);
  return count;
}

export { generateUUID };
