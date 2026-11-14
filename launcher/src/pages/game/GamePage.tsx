import { FolderOpen, FolderSync, Trash2, X } from "lucide-react";
import type { TFunction } from "i18next";
import { useTranslation } from "react-i18next";
import { JobProgress, PageHeader, Row, Section } from "@/components/common";
import { GameAction, LibraryBadge } from "@/components/games";
import { Button } from "@/components/ui";
import { useAction, useAppState } from "@/hooks";
import { api, pickFolder, reveal } from "@/lib";
import { bytes, when } from "@/lib";
import type { GameEntry } from "@/lib";
import { GameSettings } from "@/pages/game/GameSettings";
import { useUi } from "@/stores";

function statusText(t: TFunction, game: GameEntry): string {
  if (!game.found) return t("game.status.missing");
  switch (game.library.state) {
    case "ready": return t("game.status.ready");
    case "stale": return t("game.status.stale");
    default: return t("game.status.none");
  }
}

export function GamePage() {
  const { t, i18n } = useTranslation();
  const { data } = useAppState();
  const { gameId, go } = useUi();
  const add = useAction(api.addGame, t("game.folderUpdated"));
  const del = useAction(api.deleteBuilds, t("game.buildsDeleted"));
  const remove = useAction(({ id }: { id: string }) => api.removeGame(id, true));
  const cancel = useAction(api.cancelJob);
  const game = data?.games.find((g) => g.id === gameId);
  if (!data || !game) return null;
  const record = game.library.built;
  const building = data.job === `library:${game.id}`;
  return (
    <>
      <PageHeader
        kicker={`${game.info?.board} · ${game.info?.year} · ${game.info?.publisher}`}
        title={game.info?.title ?? game.id}
        actions={
          <>
            {building && <Button variant="secondary" onClick={() => cancel.mutate(undefined)}><X /> {t("common.cancel")}</Button>}
            <GameAction game={game} size="lg" />
          </>
        }
      />
      <JobProgress match={`library:${game.id}`} className="mb-5" />
      {!data.can_play && (
        <Section>
          <Row title={t("game.noPlayer")} description={t("game.noPlayerText")} />
        </Section>
      )}
      <Section title={t("game.here")}>
        <Row title={t("game.status.title")} description={statusText(t, game)}>
          <LibraryBadge game={game} />
        </Row>
        {record && (
          <Row
            title={t("game.code")}
            description={t("game.codeText", { functions: record.functions.toLocaleString(i18n.language), size: bytes(record.size), when: when(record.built), seconds: record.seconds })}
          >
            <Button variant="secondary" size="sm" disabled={building} onClick={() => del.mutate(game.id)}><Trash2 /> {t("common.delete")}</Button>
          </Row>
        )}
        <Row title={t("game.folder")} description={<span className="break-all font-mono">{game.path}</span>}>
          <Button variant="secondary" size="sm" onClick={() => reveal(game.path)}><FolderOpen /> {t("common.show")}</Button>
          <Button variant="secondary" size="sm" onClick={async () => { const p = await pickFolder(t("game.pickFolder")); if (p) add.mutate(p); }}><FolderSync /> {t("game.change")}</Button>
        </Row>
      </Section>
      <GameSettings key={game.id} game={game} />
      <Section>
        <Row title={t("game.remove")} description={t("game.removeText")}>
          <Button variant="destructive" size="sm" disabled={building || data.running === game.id} onClick={() => remove.mutate({ id: game.id }, { onSuccess: () => go("library") })}>
            <Trash2 /> {t("common.remove")}
          </Button>
        </Row>
      </Section>
    </>
  );
}
