import { useTranslation } from "react-i18next";
import { useAction } from "@/hooks/useAction";
import { api, pickFolder } from "@/lib";
import { useUi } from "@/stores";

/** Asks for a game folder, adds it and opens the game's page. */
export function useAddGame() {
  const { t } = useTranslation();
  const go = useUi((s) => s.go);
  const add = useAction(api.addGame);
  return async () => {
    const path = await pickFolder(t("library.pickFolder"));
    if (!path) return;
    add.mutate(path, { onSuccess: (r) => r && go("game", r.id) });
  };
}
