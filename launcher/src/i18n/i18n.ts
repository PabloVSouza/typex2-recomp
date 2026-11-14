// Launcher texts (react-i18next). en.json is the reference: its keys are the typed keys
// (i18next.d.ts); every other file in locales/ follows it (scripts/check-locales.mjs), and a
// missing key falls back to English.
import i18n from "i18next";
import { initReactI18next } from "react-i18next";

const files = import.meta.glob<Record<string, unknown>>("/src/i18n/locales/*.json", { eager: true, import: "default" });
const resources = Object.fromEntries(
  Object.entries(files).map(([path, translation]) => [path.replace(/^.*\/(.+)\.json$/, "$1"), { translation }]),
);

/** The launcher languages, by their own names (About → Language). */
export const LAUNCHER_LANGUAGES = [
  { code: "en", name: "English" },
  { code: "es", name: "Español" },
  { code: "fr", name: "Français" },
  { code: "pt-BR", name: "Português (Brasil)" },
  { code: "pt-PT", name: "Português (Portugal)" },
  { code: "de", name: "Deutsch" },
  { code: "ja", name: "日本語" },
  { code: "ko", name: "한국어" },
  { code: "zh-CN", name: "简体中文" },
] as const;

/** A supported language for a code ("" = the system's), English otherwise. Portuguese outside
 *  Portugal goes to Brazilian Portuguese, every Chinese to Simplified Chinese. */
export function resolveLanguage(code: string): string {
  const wanted = (code || navigator.language || "en").replace("_", "-");
  const lower = wanted.toLowerCase();
  const exact = LAUNCHER_LANGUAGES.find((l) => l.code.toLowerCase() === lower);
  if (exact) return exact.code;
  const [base, ...rest] = lower.split("-");
  if (base === "pt") return rest.join("-") === "pt" ? "pt-PT" : "pt-BR";
  if (base === "zh") return "zh-CN";
  return LAUNCHER_LANGUAGES.find((l) => l.code.split("-")[0] === base)?.code ?? "en";
}

i18n.use(initReactI18next).init({
  resources,
  lng: resolveLanguage(""),
  fallbackLng: "en",
  interpolation: { escapeValue: false },
  returnNull: false,
});

export { i18n };
