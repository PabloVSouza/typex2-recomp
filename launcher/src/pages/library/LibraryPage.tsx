import { FolderPlus, Gamepad2, MessagesSquare } from "lucide-react";
import { Trans, useTranslation } from "react-i18next";
import { Empty, PageHeader } from "@/components/common";
import { Button } from "@/components/ui";
import { useAddGame, useAppState } from "@/hooks";
import { openUrl } from "@/lib";
import { AlsoSupported } from "@/pages/library/AlsoSupported";
import { GameCard } from "@/pages/library/GameCard";

/** The project's Discord server (a permanent invite) */
const DISCORD = "https://discord.gg/bEQYMsEe9A";

export function LibraryPage() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const addGame = useAddGame();
  if (!data) return null;
  const added = new Set(data.games.map((g) => g.id));
  return (
    <>
      <PageHeader
        kicker={t("library.kicker")}
        title={t("library.title")}
        description={t("library.description")}
        actions={
          <>
            <Button variant="secondary" onClick={() => openUrl(DISCORD)} title={t("library.discordHint")}>
              <MessagesSquare /> Discord
            </Button>
            <Button onClick={addGame}><FolderPlus /> {t("library.add")}</Button>
          </>
        }
      />
      {data.games.length === 0 ? (
        <Empty icon={<Gamepad2 />} title={t("library.emptyTitle")}>
          <Trans i18nKey="library.emptyText" components={{ code: <code className="text-foreground" /> }} />
        </Empty>
      ) : (
        <div className="grid gap-4 sm:grid-cols-2">{data.games.map((g) => <GameCard key={g.id} game={g} />)}</div>
      )}
      <AlsoSupported games={data.catalog.filter((g) => !added.has(g.id))} />
    </>
  );
}
