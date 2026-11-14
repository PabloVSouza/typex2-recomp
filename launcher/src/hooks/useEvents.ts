import { useEffect } from "react";
import { useQueryClient } from "@tanstack/react-query";
import { toast } from "sonner";
import { i18n } from "@/i18n";
import { listen } from "@/lib";
import { jobLabel } from "@/lib";
import type { JobEvent } from "@/lib";
import { useActivity } from "@/stores";

/** The backend's events into the stores; the state is refreshed when a job or a game ends. */
export function useEvents() {
  const qc = useQueryClient();
  useEffect(() => {
    const { onJob, onBuildLine, onGameLine } = useActivity.getState();
    const t = i18n.t;
    const subs = [
      listen<JobEvent>("job", (e) => {
        onJob(e);
        if (e.step === "done" || e.step === "error" || e.step === "cancelled") {
          qc.invalidateQueries({ queryKey: ["state"] });
          if (e.step === "done") toast.success(t("jobs.finished", { job: jobLabel(t, e.job) }));
          if (e.step === "error") toast.error(t("jobs.failed", { job: jobLabel(t, e.job) }), { description: e.message?.split("\n")[0] });
        }
      }),
      listen<{ job: string; line: string }>("job-log", (e) => onBuildLine(e.line)),
      listen<string>("game-output", onGameLine),
      listen<{ running: string | null; code?: number | null }>("game-state", (e) => {
        qc.invalidateQueries({ queryKey: ["state"] });
        if (!e.running && e.code) toast.error(t("jobs.gameStopped", { code: e.code }));
      }),
    ];
    return () => subs.forEach((p) => p.then((off) => off()));
  }, [qc]);
}
