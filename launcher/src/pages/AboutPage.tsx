import { PageHeader, Row, Section } from "@/components/common";
import { useAppState } from "@/hooks";

export function AboutPage() {
  const { data } = useAppState();
  if (!data) return null;
  return (
    <>
      <PageHeader
        kicker="About"
        title="Type X Recomp"
        description="Static recompilation of Taito Type X and Type X2 arcade games: each game's x86 code is translated to C and compiled to native code, with Windows, Direct3D, DirectSound and the JVS I/O board provided by the runtime."
      />
      <Section title="No game files included">
        <Row title="Your games stay yours" description="The launcher, the recompiler and the runtime contain no game code or data. Games are recompiled on your computer from your own copies; what is built stays in the launcher's folder and in the exports you make." />
      </Section>
      <Section title="This installation">
        <Row title="Launcher" description={`${data.version} · ${data.os} ${data.arch}`} />
        <Row title="Kit" description={data.kit ? `${data.kit.kind === "bundled" ? "Bundled with the app" : "Repository checkout"}: ${data.kit.source}` : "Missing"} />
        <Row title="Data folder" description={<span className="break-all font-mono">{data.data}</span>} />
      </Section>
    </>
  );
}
