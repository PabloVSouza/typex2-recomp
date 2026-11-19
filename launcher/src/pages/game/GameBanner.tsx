import type { ReactNode } from "react";
import { useGameArt } from "@/hooks";
import { cn } from "@/lib";

/** The game page's header over the game's art (as the library card), when there is art. */
export function GameBanner({ id, children }: { id: string; children: ReactNode }) {
  const { data: art } = useGameArt(id);
  if (!art) return <>{children}</>;
  return (
    <div className={cn("relative isolate mb-7 overflow-hidden rounded-2xl px-5 pt-36 max-md:mb-5 max-md:pt-28 [&>header]:mb-5")}>
      <img src={art} alt="" className="absolute inset-0 -z-20 size-full object-cover object-[center_30%]" />
      <div className="absolute inset-0 -z-10 bg-gradient-to-t from-background via-background/50 to-transparent" />
      {children}
    </div>
  );
}
