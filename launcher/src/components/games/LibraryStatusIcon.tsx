import { AlertTriangle, CheckCircle2, CircleDashed, CircleOff, FolderX } from "lucide-react";
import { useTranslation } from "react-i18next";
import { useAppState } from "@/hooks";
import type { GameEntry } from "@/lib";
import { cn } from "@/lib";

/** LibraryBadge as a single icon (its text in the tooltip), for compact places such as the cards. */
export function LibraryStatusIcon({ game, className }: { game: GameEntry; className?: string }) {
  const { t } = useTranslation();
  const { data } = useAppState();
  const [Icon, color, label] = !game.supported
    ? [CircleOff, "text-muted-foreground", t("game.badge.unsupported")]
    : !game.found
      ? [FolderX, "text-destructive", t("game.badge.missing")]
      : game.library.state === "ready"
        ? [CheckCircle2, "text-lime", t("game.badge.ready")]
        : game.library.state === "stale"
          ? [AlertTriangle, "text-amber", t("game.badge.stale")]
          : [CircleDashed, "text-muted-foreground", data?.android ? t("game.badge.noPack") : t("game.badge.none")];
  return (
    <span title={label} aria-label={label} role="img" className={cn("inline-flex", color, className)}>
      <Icon className="size-5 drop-shadow" />
    </span>
  );
}
