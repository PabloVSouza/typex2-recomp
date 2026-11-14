import { useTranslation } from "react-i18next";
import { bytes } from "@/lib";
import { jobLabel, stepText } from "@/lib";
import { cn } from "@/lib";
import { useActivity } from "@/stores";

/** The running job's progress, when `match` (a job id or prefix) is the running job. */
export function JobProgress({ match, className }: { match: string; className?: string }) {
  const { t } = useTranslation();
  const job = useActivity((s) => s.job);
  if (!job || !job.job.startsWith(match) || ["done", "error", "cancelled"].includes(job.step)) return null;
  const total = job.total ?? 0;
  const done = job.done ?? 0;
  const pct = total > 0 ? Math.min(100, (done / total) * 100) : null;
  const count = total > 0 ? (job.step === "download" ? t("jobs.bytesOf", { done: bytes(done), total: bytes(total) }) : `${done} / ${total}`) : null;
  return (
    <div className={cn("surface-inset rounded-xl px-4 py-3", className)}>
      <div className="mb-2 flex items-center justify-between gap-3 text-xs">
        <span className="font-medium text-foreground">{jobLabel(t, job.job)}: {stepText(t, job)}</span>
        {count && <span className="tabular-nums text-muted-foreground">{count}</span>}
      </div>
      <div className="h-1.5 overflow-hidden rounded-full bg-white/5">
        {pct === null ? (
          <div className="h-full w-1/3 animate-pulse rounded-full bg-gradient-to-r from-cyan/30 via-cyan to-cyan/30" />
        ) : (
          <div className="h-full rounded-full bg-gradient-to-r from-amber to-pink transition-[width] duration-300" style={{ width: `${pct}%` }} />
        )}
      </div>
    </div>
  );
}
