// src/lib/currency.js
// Shopper display currency: stored choice > locale guess > USD.
// Display only — checkout always charges the skill's canonical price.

const STORAGE_KEY = "mpx_currency";

// Minimal language-tag → currency map. Anything unknown falls back to USD.
// Extend as the buyer base grows; the gateway supports ~160 currencies.
const LOCALE_CURRENCY = {
  "en-US": "USD",
  "zh-HK": "HKD", "en-HK": "HKD",
  "zh-CN": "CNY",
  "zh-SG": "SGD", "en-SG": "SGD",
  "en-GB": "GBP",
  "de-DE": "EUR", "fr-FR": "EUR", "it-IT": "EUR",
  "ja-JP": "JPY",
  "en-AU": "AUD", "en-CA": "CAD",
};

export function detectCurrency() {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved && /^[A-Za-z]{3}$/.test(saved)) return saved.toUpperCase();
  } catch { /* storage unavailable (private mode) — fall through */ }
  const tag = (navigator.language || "en-US").replace("_", "-");
  return LOCALE_CURRENCY[tag] || "USD";
}

export function setPreferredCurrency(code) {
  const c = (code || "").toUpperCase();
  if (!/^[A-Z]{3}$/.test(c)) return;
  try { localStorage.setItem(STORAGE_KEY, c); } catch { /* ignore */ }
}
