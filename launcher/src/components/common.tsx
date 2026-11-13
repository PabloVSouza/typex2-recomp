import type { ReactNode } from "react";
import { Badge, Card, CardContent, CardDescription, CardHeader, CardTitle, Select, SelectContent, SelectItem, SelectTrigger, SelectValue, Switch } from "@/components/ui";
import { useActivity } from "@/stores";
import { jobLabel } from "@/hooks";
import { bytes } from "@/lib/format";
import type { GameEntry, JobEvent } from "@/lib/types";
import { cn } from "@/lib/utils";

export function PageHeader({ title, kicker, description, actions }: { title: ReactNode; kicker?: ReactNode; description?: ReactNode; actions?: ReactNode }) {
  return (
    <header className="mb-7 flex flex-wrap items-end justify-between gap-4">
      <div className="min-w-0">
        {kicker && <div className="mb-1 font-display text-xs font-medium uppercase tracking-[0.25em] text-cyan">{kicker}</div>}
        <h1 className="font-display text-3xl font-bold tracking-wide">{title}</h1>
        {description && <p className="mt-2 max-w-2xl text-sm leading-relaxed text-muted-foreground">{description}</p>}
      </div>
      {actions && <div className="flex shrink-0 items-center gap-2">{actions}</div>}
    </header>
  );
}

/** A card of rows with an optional title. */
export function Section({ title, description, children, className }: { title?: string; description?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <Card className={cn("surface mb-5 gap-0 py-0 ring-0", className)}>
      {(title || description) && (
        <CardHeader className="px-5 pb-1 pt-4">
          {title && <CardTitle className="font-display text-xs font-semibold uppercase tracking-[0.2em] text-amber">{title}</CardTitle>}
          {description && <CardDescription className="text-xs leading-relaxed">{description}</CardDescription>}
        </CardHeader>
      )}
      <CardContent className="px-0 pb-1">{children}</CardContent>
    </Card>
  );
}

export function Row({ title, description, children, disabled }: { title: ReactNode; description?: ReactNode; children?: ReactNode; disabled?: boolean }) {
  return (
    <div className={cn("flex flex-col items-stretch gap-3 px-5 py-3.5 sm:flex-row sm:items-center sm:justify-between sm:gap-6", disabled && "pointer-events-none opacity-40")}>
      <div className="min-w-0">
        <div className="text-sm font-medium">{title}</div>
        {description && <div className="mt-0.5 text-xs leading-relaxed text-muted-foreground">{description}</div>}
      </div>
      <div className="flex min-w-0 shrink-0 flex-wrap items-center gap-2 [&>*]:max-w-full">{children}</div>
    </div>
  );
}

export function SwitchRow({ title, description, checked, onChange }: { title: ReactNode; description?: ReactNode; checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <Row title={title} description={description}>
      <Switch checked={checked} onCheckedChange={(v) => onChange(v)} />
    </Row>
  );
}

export function SelectRow<T extends string | number>({ title, description, value, options, onChange }: {
  title: ReactNode; description?: ReactNode; value: T; options: { value: T; label: string }[]; onChange: (v: T) => void;
}) {
  return (
    <Row title={title} description={description}>
      <Select value={value} items={options} onValueChange={(v) => v !== null && onChange(v as T)}>
        <SelectTrigger className="w-full bg-secondary sm:w-auto sm:min-w-56">
          <SelectValue />
        </SelectTrigger>
        <SelectContent>
          {options.map((o) => (
            <SelectItem key={String(o.value)} value={o.value}>{o.label}</SelectItem>
          ))}
        </SelectContent>
      </Select>
    </Row>
  );
}

export function LibraryBadge({ game }: { game: GameEntry }) {
  if (!game.found) return <Badge variant="destructive">Folder missing</Badge>;
  switch (game.library.state) {
    case "ready": return <Badge className="bg-lime/15 text-lime">Ready</Badge>;
    case "stale": return <Badge className="bg-amber/15 text-amber">Rebuild needed</Badge>;
    default: return <Badge variant="secondary">Not built</Badge>;
  }
}

const STEP_TEXT: Record<string, string> = {
  hash: "Checking the game's executable",
  generate: "Recompiling the executable to C",
  compile: "Compiling",
  link: "Linking",
  configure: "Configuring the build",
  build: "Building",
  download: "Downloading",
  unpack: "Unpacking",
};

function describe(e: JobEvent): string {
  const base = STEP_TEXT[e.step] ?? e.step;
  if (e.step === "download" || e.step === "unpack") return `${base} ${e.tool ?? ""}`.trim();
  return base;
}

/** The running job's progress, when `match` (a job id or prefix) is the running job. */
export function JobProgress({ match, className }: { match: string; className?: string }) {
  const job = useActivity((s) => s.job);
  if (!job || !job.job.startsWith(match) || ["done", "error", "cancelled"].includes(job.step)) return null;
  const total = job.total ?? 0;
  const done = job.done ?? 0;
  const pct = total > 0 ? Math.min(100, (done / total) * 100) : null;
  const count = total > 0 ? (job.step === "download" ? `${bytes(done)} of ${bytes(total)}` : `${done} / ${total}`) : null;
  return (
    <div className={cn("surface-inset rounded-xl px-4 py-3", className)}>
      <div className="mb-2 flex items-center justify-between gap-3 text-xs">
        <span className="font-medium text-foreground">{jobLabel(job.job)}: {describe(job)}</span>
        {count && <span className="tabular-nums text-muted-foreground">{count}</span>}
      </div>
      <div className="h-1.5 overflow-hidden rounded-full bg-white/5">
        {pct === null ? (
          <div className="h-full w-1/3 animate-pulse rounded-full bg-gradient-to-r from-cyan/30 via-cyan to-cyan/30" />
        ) : (
          <div className="h-full rounded-full bg-gradient-to-r from-amber to-pink transition-[width] duration-300" style={{ width: `${pct}%` }} />
        )}
      </div>
    </div>
  );
}

export function Empty({ icon, title, children }: { icon: ReactNode; title: string; children?: ReactNode }) {
  return (
    <div className="surface flex flex-col items-center rounded-2xl px-6 py-14 text-center">
      <div className="mb-4 text-cyan [&_svg]:size-10">{icon}</div>
      <div className="font-display text-lg font-semibold">{title}</div>
      {children && <div className="mt-2 max-w-md text-sm text-muted-foreground">{children}</div>}
    </div>
  );
}
