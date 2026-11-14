import { useTranslation } from "react-i18next";
import { SectionHeading } from "@/components/common";
import type { GameInfo } from "@/lib";

/** The supported games not in the library yet. */
export function AlsoSupported({ games }: { games: GameInfo[] }) {
  const { t } = useTranslation();
  if (games.length === 0) return null;
  return (
    <section className="mt-10">
      <SectionHeading>{t("library.alsoSupported")}</SectionHeading>
      <div className="flex flex-wrap gap-2">
        {games.map((g) => (
          <span key={g.id} className="rounded-full bg-white/[0.04] px-3 py-1.5 text-xs text-muted-foreground">
            {g.title} <span className="text-muted-foreground/60">· {g.board}</span>
          </span>
        ))}
      </div>
    </section>
  );
}
