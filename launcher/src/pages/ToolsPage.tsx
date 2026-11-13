import { useEffect, useState } from "react";
import { CheckCircle2, CircleDashed, Download, FolderOpen, Trash2, X } from "lucide-react";
import { Badge, Button, Input } from "@/components/ui";
import { JobProgress, PageHeader, Row, Section } from "@/components/common";
import { useAction, useAppState } from "@/hooks";
import { api, reveal } from "@/lib/api";
import { bytes } from "@/lib/format";

const SOURCE: Record<string, string> = { kit: "Part of this app", downloaded: "Downloaded", system: "On this computer" };

export function ToolsPage() {
  const { data } = useAppState();
  const install = useAction(api.installTools);
  const remove = useAction(api.removeTools, "Build tools removed");
  const cancel = useAction(api.cancelJob);
  const saveBuild = useAction(({ ndk, jobs }: { ndk: string; jobs: number }) => api.saveBuildSettings(ndk, jobs));
  const [ndk, setNdk] = useState("");
  const [jobs, setJobs] = useState("0");
  useEffect(() => {
    if (data) { setNdk(data.ndk); setJobs(String(data.jobs)); }
  }, [data?.ndk, data?.jobs]); // eslint-disable-line react-hooks/exhaustive-deps
  if (!data) return null;
  const missing = data.tools.filter((t) => t.download && t.source !== "downloaded").map((t) => t.download!);
  const pkg = (id: string | null) => data.packages.find((p) => p.id === id);
  return (
    <>
      <PageHeader
        kicker="Build tools"
        title="What building needs"
        description="The launcher downloads its compilers into its own folder: nothing is installed on your computer, and Remove takes it all away. Tools you already have are used when nothing was downloaded."
        actions={
          data.job === "tools"
            ? <Button variant="secondary" onClick={() => cancel.mutate(undefined)}><X /> Cancel</Button>
            : missing.length > 0 && <Button disabled={!!data.job} onClick={() => install.mutate(missing)}><Download /> Download all</Button>
        }
      />
      <JobProgress match="tools" className="mb-5" />
      <Section>
        {data.tools.map((t) => {
          const p = pkg(t.download);
          return (
            <Row
              key={t.id}
              title={
                <span className="flex items-center gap-2">
                  {t.path ? <CheckCircle2 className="size-4 text-lime" /> : <CircleDashed className="size-4 text-muted-foreground" />}
                  {t.name}
                  {p && <span className="text-xs font-normal text-muted-foreground">{p.version}</span>}
                </span>
              }
              description={
                <>
                  <span>For: {t.for_what}.</span>{" "}
                  {t.path ? <span className="break-all font-mono">{t.path}</span> : <span>{p ? `Download${p.size ? ` (${bytes(p.size)})` : ""}, or: ` : ""}{t.hint}</span>}
                </>
              }
            >
              {t.source && <Badge variant="secondary">{SOURCE[t.source]}</Badge>}
              {p && t.source !== "downloaded" && (
                <Button size="sm" variant="secondary" disabled={!!data.job} onClick={() => install.mutate([p.id])}><Download /> Download</Button>
              )}
            </Row>
          );
        })}
      </Section>
      <Section title="Storage">
        <Row title="Downloaded tools" description={`${bytes(data.tools_size)} in the launcher's folder.`}>
          <Button variant="secondary" size="sm" onClick={() => reveal(data.data)}><FolderOpen /> Show</Button>
          <Button variant="destructive" size="sm" disabled={!!data.job || data.tools_size === 0} onClick={() => remove.mutate(undefined)}><Trash2 /> Remove</Button>
        </Row>
      </Section>
      <Section title="Building">
        <Row title="Parallel jobs" description="Compilers running at once. 0 uses every core but two, so the computer stays responsive.">
          <Input className="w-20 text-right" inputMode="numeric" value={jobs} onChange={(e) => setJobs(e.target.value.replace(/\D/g, ""))} onBlur={() => saveBuild.mutate({ ndk, jobs: Number(jobs) || 0 })} />
        </Row>
        <Row title="Android NDK folder" description="Only when an NDK you installed yourself is not found by itself.">
          <Input className="w-80 font-mono text-xs" value={ndk} placeholder="~/Library/Android/sdk/ndk/30.0.16248370" onChange={(e) => setNdk(e.target.value)} onBlur={() => saveBuild.mutate({ ndk, jobs: Number(jobs) || 0 })} />
        </Row>
      </Section>
    </>
  );
}
