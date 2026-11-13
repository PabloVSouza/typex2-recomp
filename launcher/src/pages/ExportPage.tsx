import { useState } from "react";
import { Boxes, Check, FolderOpen, Smartphone, Monitor, X } from "lucide-react";
import { Badge, Button } from "@/components/ui";
import { Empty, JobProgress, PageHeader, Row, Section } from "@/components/common";
import { useAction, useAppState } from "@/hooks";
import { api, reveal } from "@/lib/api";
import { when } from "@/lib/format";
import { useUi } from "@/stores";
import { cn } from "@/lib/utils";

export function ExportPage() {
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
  const t = data.targets.find((x) => x.id === target)!;
  const missing = t.needs.map((id) => data.tools.find((tool) => tool.id === id)!).filter((tool) => tool && !tool.path);
  const downloadable = missing.filter((tool) => tool.download).map((tool) => tool.download!);
  const toggle = (id: string) => {
    const next = new Set(chosen);
    if (next.has(id)) next.delete(id); else next.add(id);
    setPicked(next);
  };
  return (
    <>
      <PageHeader
        kicker="Export"
        title="Build for another platform"
        description="Recompile your games into a RetroArch core or the Android app. Everything is built here, from your own files; copy the result to your device."
      />
      <div className="mb-5 grid gap-3 sm:grid-cols-3">
        {data.targets.map((x) => (
          <button
            key={x.id}
            type="button"
            onClick={() => setTarget(x.id)}
            className={cn("surface rounded-2xl p-4 text-left ring-1 transition", x.id === target ? "ring-cyan/60" : "ring-transparent hover:bg-white/[0.03]")}
          >
            <div className="mb-2 text-cyan [&_svg]:size-5">{x.id.startsWith("android") ? <Smartphone /> : <Monitor />}</div>
            <div className="font-display font-semibold">{x.name}</div>
          </button>
        ))}
      </div>
      <Section title={t.name} description={t.description}>
        {missing.length > 0 && (
          <Row title="Tools needed" description={missing.map((m) => m.name).join(", ") + (downloadable.length < missing.length ? ". Some must be installed separately (see Build tools)." : ".")}>
            {downloadable.length > 0 && <Button disabled={!!data.job} onClick={() => install.mutate(downloadable)}>Download</Button>}
            <Button variant="secondary" onClick={() => go("tools")}>Build tools</Button>
          </Row>
        )}
        <JobProgress match="tools" className="mx-5 mb-3" />
        <div className="px-5 pb-2 pt-2 text-sm font-medium">Games</div>
        {games.length === 0 ? (
          <div className="px-5 pb-4 text-sm text-muted-foreground">Add games in the Library first.</div>
        ) : (
          <div className="grid gap-2 px-5 pb-4 sm:grid-cols-2">
            {games.map((g) => (
              <button
                key={g.id}
                type="button"
                onClick={() => toggle(g.id)}
                className={cn("flex items-center gap-3 rounded-lg px-3 py-2.5 text-left text-sm transition", chosen.has(g.id) ? "bg-cyan/10" : "bg-white/[0.03] text-muted-foreground")}
              >
                <span className={cn("grid size-4 shrink-0 place-items-center rounded border", chosen.has(g.id) ? "border-cyan bg-cyan text-[#0b0a14]" : "border-input")}>
                  {chosen.has(g.id) && <Check className="size-3" strokeWidth={3} />}
                </span>
                <span className="truncate">{g.info?.title}</span>
              </button>
            ))}
          </div>
        )}
        <JobProgress match={`export:${t.id}`} className="mx-5 mb-4" />
        <div className="flex justify-end gap-2 px-5 pb-4">
          {data.job?.startsWith("export:") && <Button variant="secondary" onClick={() => cancel.mutate(undefined)}><X /> Cancel</Button>}
          <Button disabled={!!data.job || chosen.size === 0 || missing.length > 0} onClick={() => start.mutate({ t: t.id, ids: [...chosen] })}>
            <Boxes /> Export {chosen.size} {chosen.size === 1 ? "game" : "games"}
          </Button>
        </div>
      </Section>
      <h2 className="mb-3 mt-8 font-display text-xs font-semibold uppercase tracking-[0.2em] text-muted-foreground">Last exports</h2>
      {data.exports.length === 0 ? (
        <Empty icon={<Boxes />} title="Nothing exported yet" />
      ) : (
        <Section>
          {data.exports.map((e) => (
            <Row
              key={e.target}
              title={data.targets.find((x) => x.id === e.target)?.name ?? e.target}
              description={`${e.games.map((id) => data.catalog.find((g) => g.id === id)?.title ?? id).join(", ")} · ${when(e.built)}`}
            >
              <Badge variant="secondary">{e.files[0]?.split(/[\\/]/).pop()}</Badge>
              <Button variant="secondary" size="sm" onClick={() => e.files[0] && reveal(e.files[0])}><FolderOpen /> Show</Button>
            </Row>
          ))}
        </Section>
      )}
    </>
  );
}
