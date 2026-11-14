import { useEffect, useState } from "react";
import { FolderOpen, FolderSync, Trash2, X } from "lucide-react";
import { Button } from "@/components/ui";
import { LayoutEditor } from "@/components/LayoutEditor";
import { layoutOf, padOf } from "@/lib/controls";
import { JobProgress, LibraryBadge, PageHeader, Row, Section, SelectRow, SwitchRow } from "@/components/common";
import { GameAction } from "@/pages/LibraryPage";
import { useAction, useAppState } from "@/hooks";
import { api, pickFolder, reveal } from "@/lib/api";
import { bytes, when } from "@/lib/format";
import type { GameEntry, GameSettings } from "@/lib/types";
import { useUi } from "@/stores";

const CUSTOM = "__custom";

function Settings({ game }: { game: GameEntry }) {
  const save = useAction((s: GameSettings) => api.saveGameSettings(game.id, s));
  const [s, setS] = useState(game.settings);
  useEffect(() => setS(game.settings), [game.id, game.settings]);
  const set = (patch: Partial<GameSettings>) => {
    const next = { ...s, ...patch };
    setS(next);
    save.mutate(next);
  };
  const layouts = game.info?.layouts ?? [];
  const same = (a: string, b: string) => padOf(layoutOf(game.id, a)) === padOf(layoutOf(game.id, b));
  const preset = layouts.find((l) => same(l.pad, s.pad));
  return (
    <>
      <Section title="Display">
        <SwitchRow title="Full screen" checked={s.fullscreen} onChange={(fullscreen) => set({ fullscreen })} />
        <SwitchRow title="Vsync" description="Present in step with the display. Off, the game paces itself at its own 60 Hz." checked={s.vsync} onChange={(vsync) => set({ vsync })} />
        <SwitchRow title="FPS counter" description="Starts with the frame-rate overlay on." checked={s.fps} onChange={(fps) => set({ fps })} />
        <SelectRow
          title="Render scale"
          description="The game's frame rendered at a multiple of its size. Automatic picks the largest that fits 1280×1024."
          value={s.scale}
          options={[{ value: 0, label: "Automatic" }, { value: 1, label: "1× (original)" }, { value: 2, label: "2×" }, { value: 3, label: "3×" }, { value: 4, label: "4×" }]}
          onChange={(scale) => set({ scale })}
        />
      </Section>
      <Section title="Controls" description="Which controller button presses each of the cabinet's buttons. The keyboard works too (see Controls in the docs).">
        <SelectRow
          title="Layout"
          description="A starting point; change single buttons below."
          value={preset ? preset.pad : CUSTOM}
          options={[...layouts.map((l) => ({ value: l.pad, label: l.label })), ...(preset ? [] : [{ value: CUSTOM, label: "Custom" }])]}
          onChange={(v) => v !== CUSTOM && set({ pad: v })}
        />
        <LayoutEditor game={game.id} pad={s.pad} onChange={(pad) => set({ pad })} />
      </Section>
      <Section title="Advanced">
        <SelectRow
          title="Log detail"
          value={s.log_level}
          options={[{ value: 0, label: "Normal" }, { value: 1, label: "Verbose (-v)" }, { value: 2, label: "More (-vv)" }, { value: 3, label: "Everything (-vvv)" }]}
          onChange={(log_level) => set({ log_level })}
        />
        <div className="px-5 pb-4 pt-2">
          <div className="text-sm font-medium">Environment</div>
          <div className="mb-2 mt-0.5 text-xs text-muted-foreground">Extra runtime switches, one KEY=VALUE per line (see Debugging → Switches in the docs).</div>
          <textarea
            className="h-24 w-full resize-y rounded-lg border border-input bg-secondary px-3 py-2 font-mono text-xs outline-none focus-visible:border-ring focus-visible:ring-3 focus-visible:ring-ring/50"
            value={s.env}
            spellCheck={false}
            placeholder="X2_TOUCH=0"
            onChange={(e) => setS({ ...s, env: e.target.value })}
            onBlur={() => save.mutate(s)}
          />
        </div>
      </Section>
    </>
  );
}

export function GamePage() {
  const { data } = useAppState();
  const { gameId, go } = useUi();
  const add = useAction(api.addGame, "Folder updated");
  const del = useAction(api.deleteBuilds, "Builds deleted");
  const remove = useAction(({ id }: { id: string }) => api.removeGame(id, true));
  const cancel = useAction(api.cancelJob);
  const game = data?.games.find((g) => g.id === gameId);
  if (!data || !game) return null;
  const record = game.library.built;
  const building = data.job === `library:${game.id}`;
  return (
    <>
      <PageHeader
        kicker={`${game.info?.board} · ${game.info?.year} · ${game.info?.publisher}`}
        title={game.info?.title ?? game.id}
        actions={
          <>
            {building && <Button variant="secondary" onClick={() => cancel.mutate(undefined)}><X /> Cancel</Button>}
            <GameAction game={game} size="lg" />
          </>
        }
      />
      <JobProgress match={`library:${game.id}`} className="mb-5" />
      {!data.can_play && (
        <Section>
          <Row title="Playing here is not available on this computer yet" description="There is no x2-player for this system. You can still export the game for Android or RetroArch." />
        </Section>
      )}
      <Section title="This computer">
        <Row title="Status" description={statusText(game)}>
          <LibraryBadge game={game} />
        </Row>
        {record && (
          <Row title="Recompiled code" description={`${record.functions.toLocaleString()} functions · ${bytes(record.size)} · built ${when(record.built)} in ${record.seconds} s`}>
            <Button variant="secondary" size="sm" disabled={building} onClick={() => del.mutate(game.id)}><Trash2 /> Delete</Button>
          </Row>
        )}
        <Row title="Game folder" description={<span className="break-all font-mono">{game.path}</span>}>
          <Button variant="secondary" size="sm" onClick={() => reveal(game.path)}><FolderOpen /> Show</Button>
          <Button variant="secondary" size="sm" onClick={async () => { const p = await pickFolder("Choose the game's folder"); if (p) add.mutate(p); }}><FolderSync /> Change</Button>
        </Row>
      </Section>
      <Settings game={game} />
      <Section>
        <Row title="Remove from the library" description="Forgets this folder and deletes what was built from it. The game's own files are not touched.">
          <Button variant="destructive" size="sm" disabled={building || data.running === game.id} onClick={() => remove.mutate({ id: game.id }, { onSuccess: () => go("library") })}>
            <Trash2 /> Remove
          </Button>
        </Row>
      </Section>
    </>
  );
}

function statusText(game: GameEntry): string {
  if (!game.found) return "The folder is gone or no longer holds this game. Point the launcher at it again below.";
  switch (game.library.state) {
    case "ready": return "Recompiled for this computer and ready to play.";
    case "stale": return "Built from another copy of the game or by an older launcher: build it again.";
    default: return "Not recompiled yet. Building takes a minute or two and uses only your own copy of the game.";
  }
}
