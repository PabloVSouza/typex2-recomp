import { FolderPlus, Gamepad2, Hammer, MessagesSquare, Play, Square } from "lucide-react";
import { Button } from "@/components/ui";
import { Empty, JobProgress, LibraryBadge, PageHeader } from "@/components/common";
import { useAction, useAppState } from "@/hooks";
import { api, openUrl, pickFolder } from "@/lib/api";
import type { GameEntry } from "@/lib/types";
import { useUi } from "@/stores";
import { cn } from "@/lib/utils";

/** The project's Discord server (a permanent invite) */
const DISCORD = "https://discord.gg/bEQYMsEe9A";

export function useAddGame() {
  const go = useUi((s) => s.go);
  const add = useAction(api.addGame);
  return async () => {
    const path = await pickFolder("Choose a game folder (the one holding game.exe)");
    if (!path) return;
    add.mutate(path, { onSuccess: (r) => r && go("game", r.id) });
  };
}

/** Build, Play or Stop: the game's next step on this computer. */
export function GameAction({ game, size = "default" }: { game: GameEntry; size?: "default" | "lg" }) {
  const { data } = useAppState();
  const build = useAction(api.buildLibrary);
  const launch = useAction(api.launch);
  const stop = useAction(api.stop);
  if (!data?.can_play) return null;
  const big = size === "lg" ? "h-11 px-5 text-base" : "";
  if (data.running === game.id) {
    return <Button variant="destructive" className={big} onClick={() => stop.mutate(undefined)}><Square /> Stop</Button>;
  }
  if (game.library.state === "ready") {
    return (
      <Button className={cn(big, "font-semibold")} disabled={!game.found || !!data.running} onClick={() => launch.mutate(game.id)}>
        <Play className="fill-current" /> Play
      </Button>
    );
  }
  return (
    <Button variant="secondary" className={big} disabled={!game.found || !!data.job} onClick={() => build.mutate(game.id)}>
      <Hammer /> {game.library.state === "stale" ? "Rebuild" : "Build"}
    </Button>
  );
}

function GameCard({ game }: { game: GameEntry }) {
  const go = useUi((s) => s.go);
  return (
    <div className="surface group relative flex flex-col overflow-hidden rounded-2xl p-5 transition hover:bg-white/[0.03]">
      <div className="pointer-events-none absolute -right-10 -top-10 size-40 rounded-full bg-gradient-to-br from-amber/15 to-pink/5 blur-2xl transition group-hover:from-amber/25" />
      <button type="button" className="relative text-left" onClick={() => go("game", game.id)}>
        <div className="mb-1 flex items-center gap-2 text-[11px] uppercase tracking-wider text-muted-foreground">
          <span className="text-cyan">{game.info?.board}</span> · {game.info?.year} · {game.info?.publisher}
        </div>
        <div className="font-display text-lg font-semibold leading-snug">{game.info?.title ?? game.id}</div>
        <div className="mt-1 truncate text-xs text-muted-foreground" title={game.path}>{game.path}</div>
      </button>
      <JobProgress match={`library:${game.id}`} className="relative mt-4" />
      <div className="relative mt-auto flex items-center justify-between gap-3 pt-5">
        <LibraryBadge game={game} />
        <GameAction game={game} />
      </div>
    </div>
  );
}

export function LibraryPage() {
  const { data } = useAppState();
  const addGame = useAddGame();
  if (!data) return null;
  const added = new Set(data.games.map((g) => g.id));
  const missing = data.catalog.filter((g) => !added.has(g.id));
  return (
    <>
      <PageHeader
        kicker="Library"
        title="Your arcade"
        description="Add the folders of the games you own. The launcher recompiles each one on this computer from your own files, to play here or to export for Android and RetroArch."
        actions={
          <>
            <Button variant="secondary" onClick={() => openUrl(DISCORD)} title="Help, news and stick layouts">
              <MessagesSquare /> Discord
            </Button>
            <Button onClick={addGame}><FolderPlus /> Add game folder</Button>
          </>
        }
      />
      {data.games.length === 0 ? (
        <Empty icon={<Gamepad2 />} title="No games yet">
          Choose a game's folder: the one with its <code className="text-foreground">game.exe</code>. The launcher recognises the supported games by their executable.
        </Empty>
      ) : (
        <div className="grid gap-4 sm:grid-cols-2">{data.games.map((g) => <GameCard key={g.id} game={g} />)}</div>
      )}
      {missing.length > 0 && (
        <section className="mt-10">
          <h2 className="mb-3 font-display text-xs font-semibold uppercase tracking-[0.2em] text-muted-foreground">Also supported</h2>
          <div className="flex flex-wrap gap-2">
            {missing.map((g) => (
              <span key={g.id} className="rounded-full bg-white/[0.04] px-3 py-1.5 text-xs text-muted-foreground">
                {g.title} <span className="text-muted-foreground/60">· {g.board}</span>
              </span>
            ))}
          </div>
        </section>
      )}
    </>
  );
}
