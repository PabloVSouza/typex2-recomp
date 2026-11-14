import { useTranslation } from "react-i18next";
import { Badge } from "@/components/ui";
import type { GameEntry } from "@/lib";

/** Whether a game is ready to play here. */
export function LibraryBadge({ game }: { game: GameEntry }) {
  const { t } = useTranslation();
  if (!game.found) return <Badge variant="destructive">{t("game.badge.missing")}</Badge>;
  switch (game.library.state) {
    case "ready": return <Badge className="bg-lime/15 text-lime">{t("game.badge.ready")}</Badge>;
    case "stale": return <Badge className="bg-amber/15 text-amber">{t("game.badge.stale")}</Badge>;
    default: return <Badge variant="secondary">{t("game.badge.none")}</Badge>;
  }
}
