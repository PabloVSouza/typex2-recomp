import { Hammer, Play, Square } from "lucide-react";
import { useTranslation } from "react-i18next";
import { Button } from "@/components/ui";
import { useAction, useAppState } from "@/hooks";
import { api } from "@/lib";
import type { GameEntry } from "@/lib";
import { cn } from "@/lib";

/** Build, Play or Stop: the game's next step on this computer. */
export function GameAction({ game, size = "default", tabIndex }: { game: GameEntry; size?: "default" | "lg"; tabIndex?: number }) {
  const { t } = useTranslation();
  const { data } = useAppState();
  const build = useAction(api.buildLibrary);
  const launch = useAction(api.launch);
  const stop = useAction(api.stop);
  if (!data?.can_play || !game.supported) return null;
  const big = size === "lg" ? "h-11 px-5 text-base" : "";
  if (data.running === game.id) {
    return <Button variant="destructive" tabIndex={tabIndex} className={big} onClick={() => stop.mutate(undefined)}><Square /> {t("game.stop")}</Button>;
  }
  if (game.library.state === "ready") {
    return (
      <Button tabIndex={tabIndex} className={cn(big, "font-semibold")} disabled={!game.found || !!data.running} onClick={() => launch.mutate(game.id)}>
        <Play className="fill-current" /> {t("game.play")}
      </Button>
    );
  }
  if (data.android) return null; // the pack comes from the launcher on a computer
  return (
    <Button variant="secondary" tabIndex={tabIndex} className={big} disabled={!game.found || !!data.job} onClick={() => build.mutate(game.id)}>
      <Hammer /> {game.library.state === "stale" ? t("game.rebuild") : t("game.build")}
    </Button>
  );
}
