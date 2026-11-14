import { i18n } from "@/i18n";

export function bytes(n: number): string {
  if (n >= 1e9) return `${(n / 1e9).toFixed(1)} GB`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(0)} MB`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(0)} KB`;
  return `${n} B`;
}

/** A Unix time as a date and time in the launcher's language. */
export function when(seconds: number): string {
  return new Date(seconds * 1000).toLocaleString(i18n.language, { dateStyle: "medium", timeStyle: "short" });
}

export function errorText(e: unknown): string {
  return typeof e === "string" ? e : e instanceof Error ? e.message : JSON.stringify(e);
}
