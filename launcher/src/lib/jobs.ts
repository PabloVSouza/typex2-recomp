import type { TFunction } from "i18next";
import type { JobEvent } from "@/lib/types";

/** A job's kind ("Build", "Export", "Download") from its id. */
export function jobLabel(t: TFunction, job: string): string {
  if (job === "tools") return t("jobs.download");
  if (job.startsWith("library:")) return t("jobs.build");
  if (job.startsWith("export:")) return t("jobs.export");
  return job;
}

/** What a job event's step is doing. */
export function stepText(t: TFunction, e: JobEvent): string {
  switch (e.step) {
    case "hash": return t("steps.hash");
    case "generate": return t("steps.generate");
    case "compile": return t("steps.compile");
    case "link": return t("steps.link");
    case "configure": return t("steps.configure");
    case "build": return t("steps.build");
    case "download": return t("steps.download", { tool: e.tool ?? "" }).trim();
    case "unpack": return t("steps.unpack", { tool: e.tool ?? "" }).trim();
    default: return e.step;
  }
}
