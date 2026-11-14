import { JobProgress } from "@/components/common";
import { GameAction, LibraryBadge } from "@/components/games";
import type { GameEntry } from "@/lib";
import { useUi } from "@/stores";

export function GameCard({ game }: { game: GameEntry }) {
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
