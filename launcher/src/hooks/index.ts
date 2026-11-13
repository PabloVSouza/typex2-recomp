import { useEffect } from "react";
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { toast } from "sonner";
import { api, listen } from "@/lib/api";
import { errorText } from "@/lib/format";
import type { JobEvent } from "@/lib/types";
import { useActivity } from "@/stores";

export function useAppState() {
  return useQuery({ queryKey: ["state"], queryFn: api.loadState });
}

/** Runs a command, refreshes the state afterwards, and shows its error as a toast. */
export function useAction<A, R = unknown>(fn: (a: A) => Promise<R>, success?: string) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: fn,
    onSuccess: () => success && toast.success(success),
    onError: (e) => toast.error(errorText(e)),
    onSettled: () => qc.invalidateQueries({ queryKey: ["state"] }),
  });
}

/** The backend's events into the stores; the state is refreshed when a job or a game ends. */
export function useEvents() {
  const qc = useQueryClient();
  useEffect(() => {
    const { onJob, onBuildLine, onGameLine } = useActivity.getState();
    const subs = [
      listen<JobEvent>("job", (e) => {
        onJob(e);
        if (e.step === "done" || e.step === "error" || e.step === "cancelled") {
          qc.invalidateQueries({ queryKey: ["state"] });
          if (e.step === "done") toast.success(jobLabel(e.job) + " finished");
          if (e.step === "error") toast.error(jobLabel(e.job) + " failed", { description: e.message?.split("\n")[0] });
        }
      }),
      listen<{ job: string; line: string }>("job-log", (e) => onBuildLine(e.line)),
      listen<string>("game-output", onGameLine),
      listen<{ running: string | null; code?: number | null }>("game-state", (e) => {
        qc.invalidateQueries({ queryKey: ["state"] });
        if (!e.running && e.code) toast.error(`The game stopped (exit code ${e.code}). See Log.`);
      }),
    ];
    return () => subs.forEach((p) => p.then((off) => off()));
  }, [qc]);
}

export function jobLabel(job: string): string {
  if (job === "tools") return "Download";
  if (job.startsWith("library:")) return "Build";
  if (job.startsWith("export:")) return "Export";
  return job;
}
