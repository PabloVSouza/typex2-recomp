import { useTranslation } from "react-i18next";
import { toast } from "sonner";
import { useAppState } from "@/hooks/useAppState";
import { buildReport, ISSUES_NEW, openUrl } from "@/lib";
import { useActivity } from "@/stores";

/** Copies a problem report (installation, games, logs; home folders hidden) and opens a new issue. */
export function useReport() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const gameLog = useActivity((s) => s.gameLog);
  const buildLog = useActivity((s) => s.buildLog);
  return async () => {
    if (!data) return;
    try {
      await navigator.clipboard.writeText(buildReport(data, gameLog, buildLog));
    } catch {
      toast.error(t("report.copyFailed"));
      return;
    }
    toast.success(t("report.copied"), { description: t("report.copiedText") });
    await openUrl(ISSUES_NEW);
  };
}
