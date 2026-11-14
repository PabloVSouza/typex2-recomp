import { useState } from "react";
import { useAction } from "@/hooks";
import { api } from "@/lib";
import type { GameEntry, GameSettings as Settings } from "@/lib";
import { AdvancedSettings } from "@/pages/game/AdvancedSettings";
import { ControlSettings } from "@/pages/game/ControlSettings";
import { DisplaySettings } from "@/pages/game/DisplaySettings";

/** The game's settings, saved as they change. Mounted per game (key), so the saved settings are read once. */
export function GameSettings({ game }: { game: GameEntry }) {
  const save = useAction((s: Settings) => api.saveGameSettings(game.id, s));
  const [s, setS] = useState(game.settings);
  const set = (patch: Partial<Settings>) => {
    const next = { ...s, ...patch };
    setS(next);
    save.mutate(next);
  };
  const edit = (patch: Partial<Settings>) => setS({ ...s, ...patch });
  return (
    <>
      <DisplaySettings s={s} set={set} />
      <ControlSettings game={game} s={s} set={set} />
      <AdvancedSettings s={s} set={set} edit={edit} />
    </>
  );
}
