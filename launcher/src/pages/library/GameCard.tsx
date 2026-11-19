import { Building2, CalendarDays, Cpu, Gamepad2 } from "lucide-react";
import { JobProgress } from "@/components/common";
import { GameAction, InfoChip, LibraryStatusIcon } from "@/components/games";
import { useGameArt } from "@/hooks";
import type { GameEntry } from "@/lib";
import { useUi } from "@/stores";

/** A game in the library: its art (ES-DE media or a libretro thumbnail) behind the title, the board,
 *  year and publisher as chips, the status as an icon and the next action. */
export function GameCard({ game }: { game: GameEntry }) {
  const go = useUi((s) => s.go);
  const { data: art } = useGameArt(game.id);
  const info = game.info;
  return (
    <div className="surface group relative isolate flex min-h-48 flex-col overflow-hidden rounded-2xl">
      {art ? (
        <img src={art} alt="" className="absolute inset-0 -z-20 size-full object-cover transition duration-500 group-hover:scale-105" />
      ) : (
        <div className="absolute inset-0 -z-20 flex items-center justify-center bg-gradient-to-br from-amber/15 via-transparent to-cyan/10">
          <Gamepad2 className="size-16 text-white/10" />
        </div>
      )}
      <div className="absolute inset-0 -z-10 bg-gradient-to-t from-black/90 via-black/45 to-black/20" />
      <div className="flex items-start justify-between gap-2 p-3">
        {info ? (
          <div className="flex flex-wrap gap-1">
            <InfoChip icon={<Cpu />} title={info.board}>{info.board.replace(/^Type /, "")}</InfoChip>
            <InfoChip icon={<CalendarDays />}>{info.year}</InfoChip>
            <InfoChip icon={<Building2 />}>{info.publisher}</InfoChip>
          </div>
        ) : (
          <span />
        )}
        <LibraryStatusIcon game={game} />
      </div>
      <button type="button" className="flex-1 cursor-pointer" aria-label={game.title} onClick={() => go("game", game.id)} />
      <div className="flex items-end justify-between gap-3 p-4 pt-0">
        <button type="button" className="min-w-0 text-left" onClick={() => go("game", game.id)}>
          <div className="font-display text-lg font-semibold leading-snug text-white drop-shadow-md">{game.title}</div>
        </button>
        <GameAction game={game} />
      </div>
      <JobProgress match={`library:${game.id}`} className="mx-4 mb-4" />
    </div>
  );
}
