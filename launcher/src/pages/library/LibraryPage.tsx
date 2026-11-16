import { useEffect } from "react";
import { FolderPlus, FolderSearch, Gamepad2, MessagesSquare, RefreshCw, ShieldCheck } from "lucide-react";
import { Trans, useTranslation } from "react-i18next";
import { Empty, PageHeader, Row, Section } from "@/components/common";
import { Button } from "@/components/ui";
import { useAction, useAddGame, useAppState, useScanFolder } from "@/hooks";
import { api, openUrl, queryClient } from "@/lib";
import { GameCard } from "@/pages/library/GameCard";

const refresh = () => queryClient.invalidateQueries({ queryKey: ["state"] });

/** The project's Discord server (a permanent invite) */
const DISCORD = "https://discord.gg/bEQYMsEe9A";

export function LibraryPage() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const addGame = useAddGame();
  const scanFolder = useScanFolder();
  const allow = useAction(api.allowAllFiles);
  const onAndroid = !!data?.android;
  // Android: back from the permission screen, a game or a file manager, the folder may have changed
  useEffect(() => {
    if (!onAndroid) return;
    const back = () => document.visibilityState === "visible" && void refresh();
    document.addEventListener("visibilitychange", back);
    return () => document.removeEventListener("visibilitychange", back);
  }, [onAndroid]);
  if (!data) return null;
  const android = data.android;
  const discord = (
    <Button variant="secondary" onClick={() => openUrl(DISCORD)} title={t("library.discordHint")}>
      <MessagesSquare /> Discord
    </Button>
  );
  return (
    <>
      <PageHeader
        kicker={t("library.kicker")}
        title={t("library.title")}
        description={android ? t("library.android.description", { path: android.roms }) : t("library.description")}
        actions={
          android ? (
            <>
              {discord}
              <Button variant="secondary" onClick={refresh}><RefreshCw /> {t("library.android.refresh")}</Button>
            </>
          ) : (
            <>
              {discord}
              <Button variant="secondary" onClick={scanFolder} title={t("library.scanHint")}><FolderSearch /> {t("library.scan")}</Button>
              <Button onClick={addGame}><FolderPlus /> {t("library.add")}</Button>
            </>
          )
        }
      />
      {android && !android.all_files && (
        <Section>
          <Row title={t("library.android.allowTitle")} description={t("library.android.allowText")}>
            <Button onClick={() => allow.mutate(undefined)}><ShieldCheck /> {t("library.android.allow")}</Button>
          </Row>
        </Section>
      )}
      {data.games.length === 0 ? (
        <Empty icon={<Gamepad2 />} title={t("library.emptyTitle")}>
          {android ? (
            <Trans i18nKey="library.android.emptyText" values={{ path: android.roms }} components={{ code: <code className="text-foreground" /> }} />
          ) : (
            <>
              <Trans i18nKey="library.emptyText" components={{ code: <code className="text-foreground" /> }} />
              <div className="mt-5 flex flex-wrap justify-center gap-2">
                <Button variant="secondary" onClick={scanFolder}><FolderSearch /> {t("library.scan")}</Button>
                <Button onClick={addGame}><FolderPlus /> {t("library.add")}</Button>
              </div>
            </>
          )}
        </Empty>
      ) : (
        <div className="grid gap-4 sm:grid-cols-2">{data.games.map((g) => <GameCard key={g.id} game={g} />)}</div>
      )}
    </>
  );
}
