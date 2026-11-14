import { useState } from "react";
import { Boxes, X } from "lucide-react";
import { useTranslation } from "react-i18next";
import { JobProgress, PageHeader, Row, Section } from "@/components/common";
import { Button } from "@/components/ui";
import { useAction, useAppState } from "@/hooks";
import { api } from "@/lib";
import { targetDescription, targetName, toolName } from "@/lib/data";
import { GamePick } from "@/pages/export/GamePick";
import { LastExports } from "@/pages/export/LastExports";
import { TargetCard } from "@/pages/export/TargetCard";
import { useUi } from "@/stores";

export function ExportPage() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const go = useUi((s) => s.go);
  const [target, setTarget] = useState("android-retroarch");
  const [picked, setPicked] = useState<Set<string> | null>(null);
  const start = useAction(({ t, ids }: { t: string; ids: string[] }) => api.exportTarget(t, ids));
  const install = useAction(api.installTools);
  const cancel = useAction(api.cancelJob);
  if (!data) return null;
  const games = data.games.filter((g) => g.found);
  const chosen = picked ?? new Set(games.map((g) => g.id));
  const x = data.targets.find((y) => y.id === target)!;
  const missing = x.needs.map((id) => data.tools.find((tool) => tool.id === id)!).filter((tool) => tool && !tool.path);
  const downloadable = missing.filter((tool) => tool.download).map((tool) => tool.download!);
  const toggle = (id: string) => {
    const next = new Set(chosen);
    if (next.has(id)) next.delete(id); else next.add(id);
    setPicked(next);
  };
  const names = missing.map((m) => toolName(t, m)).join(", ");
  return (
    <>
      <PageHeader kicker={t("export.kicker")} title={t("export.title")} description={t("export.description")} />
      <div className="mb-5 grid gap-3 sm:grid-cols-3">
        {data.targets.map((y) => <TargetCard key={y.id} target={y} active={y.id === target} onClick={() => setTarget(y.id)} />)}
      </div>
      <Section title={targetName(t, x)} description={targetDescription(t, x)}>
        {missing.length > 0 && (
          <Row title={t("export.needed")} description={downloadable.length < missing.length ? t("export.neededSome", { tools: names }) : t("export.neededAll", { tools: names })}>
            {downloadable.length > 0 && <Button disabled={!!data.job} onClick={() => install.mutate(downloadable)}>{t("tools.download")}</Button>}
            <Button variant="secondary" onClick={() => go("tools")}>{t("nav.tools")}</Button>
          </Row>
        )}
        <JobProgress match="tools" className="mx-5 mb-3" />
        <div className="px-5 pb-2 pt-2 text-sm font-medium">{t("nav.games")}</div>
        {games.length === 0 ? (
          <div className="px-5 pb-4 text-sm text-muted-foreground">{t("export.noGames")}</div>
        ) : (
          <div className="grid gap-2 px-5 pb-4 sm:grid-cols-2">
            {games.map((g) => <GamePick key={g.id} title={g.info?.title ?? g.id} on={chosen.has(g.id)} onClick={() => toggle(g.id)} />)}
          </div>
        )}
        <JobProgress match={`export:${x.id}`} className="mx-5 mb-4" />
        <div className="flex justify-end gap-2 px-5 pb-4">
          {data.job?.startsWith("export:") && <Button variant="secondary" onClick={() => cancel.mutate(undefined)}><X /> {t("common.cancel")}</Button>}
          <Button disabled={!!data.job || chosen.size === 0 || missing.length > 0} onClick={() => start.mutate({ t: x.id, ids: [...chosen] })}>
            <Boxes /> {t("export.start", { count: chosen.size })}
          </Button>
        </div>
      </Section>
      <LastExports data={data} />
    </>
  );
}
