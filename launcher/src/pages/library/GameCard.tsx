import { useEffect, useRef } from "react";
import { Building2, CalendarDays, Cpu, Gamepad2 } from "lucide-react";
import { JobProgress } from "@/components/common";
import { GameAction, InfoChip, LibraryStatusIcon } from "@/components/games";
import { useAction, useAppState, useGameArt } from "@/hooks";
import { api } from "@/lib";
import type { GameEntry } from "@/lib";
import { useUi } from "@/stores";

/** A game in the library: its art (ES-DE media or a libretro thumbnail) behind the title, the board,
 *  year and publisher as chips, the status as an icon and the next action. With a controller (or
 *  the keyboard) the card is one stop: A / Enter plays the game (its page when it can't start
 *  here), X opens its page; by touch or mouse, the card opens the page and Play plays. */
export function GameCard({ game }: { game: GameEntry }) {
  const go = useUi((s) => s.go);
  const { data } = useAppState();
  const { data: art } = useGameArt(game.id);
  const launch = useAction(api.launch);
  const ref = useRef<HTMLDivElement>(null);
  const info = game.info;
  const playable = !!data?.can_play && game.supported && game.found && game.library.state === "ready" && !data.running;
  const open = () => go("game", game.id);
  const play = () => (playable ? launch.mutate(game.id) : open());

  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const onActivate = (e: Event) => {
      e.preventDefault();
      play();
    };
    const onSecondary = (e: Event) => {
      e.preventDefault();
      open();
    };
    el.addEventListener("padactivate", onActivate);
    el.addEventListener("padsecondary", onSecondary);
    return () => {
      el.removeEventListener("padactivate", onActivate);
      el.removeEventListener("padsecondary", onSecondary);
    };
  });

  return (
    <div
      ref={ref}
      tabIndex={0}
      data-card
      role="group"
      aria-label={game.title}
      onClick={(e) => e.target === e.currentTarget && open()}
      onKeyDown={(e) => {
        if (e.target !== e.currentTarget || e.defaultPrevented) return;
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          play();
        }
      }}
      className="surface group relative isolate flex min-h-48 cursor-pointer flex-col overflow-hidden rounded-2xl outline-none"
    >
      {art ? (
        <img src={art} alt="" className="pointer-events-none absolute inset-0 -z-20 size-full object-cover transition duration-500 group-hover:scale-105 group-focus:scale-105" />
      ) : (
        <div className="pointer-events-none absolute inset-0 -z-20 flex items-center justify-center bg-gradient-to-br from-amber/15 via-transparent to-cyan/10">
          <Gamepad2 className="size-16 text-white/10" />
        </div>
      )}
      <div className="pointer-events-none absolute inset-0 -z-10 bg-gradient-to-t from-black/90 via-black/45 to-black/20" />
      <div className="pointer-events-none flex items-start justify-between gap-2 p-3">
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
      <div className="pointer-events-none flex-1" />
      <div className="flex items-end justify-between gap-3 p-4 pt-0">
        <button type="button" tabIndex={-1} className="min-w-0 text-left" onClick={open}>
          <div className="font-display text-lg font-semibold leading-snug text-white drop-shadow-md">{game.title}</div>
        </button>
        <GameAction game={game} tabIndex={-1} />
      </div>
      <JobProgress match={`library:${game.id}`} className="mx-4 mb-4" />
    </div>
  );
}
