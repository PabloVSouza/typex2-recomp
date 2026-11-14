import { useState } from "react";
import { useTranslation } from "react-i18next";
import { Row, Section } from "@/components/common";
import { Input } from "@/components/ui";
import { useAction } from "@/hooks";
import { api } from "@/lib";

/** Parallel jobs and the NDK folder, saved when a field loses focus. Keyed by the saved values. */
export function BuildSettings({ ndk: savedNdk, jobs: savedJobs }: { ndk: string; jobs: number }) {
  const { t } = useTranslation();
  const save = useAction(({ ndk, jobs }: { ndk: string; jobs: number }) => api.saveBuildSettings(ndk, jobs));
  const [ndk, setNdk] = useState(savedNdk);
  const [jobs, setJobs] = useState(String(savedJobs));
  const commit = () => save.mutate({ ndk, jobs: Number(jobs) || 0 });
  return (
    <Section title={t("tools.building")}>
      <Row title={t("tools.jobs")} description={t("tools.jobsText")}>
        <Input className="w-20 text-right" inputMode="numeric" value={jobs} onChange={(e) => setJobs(e.target.value.replace(/\D/g, ""))} onBlur={commit} />
      </Row>
      <Row title={t("tools.ndk")} description={t("tools.ndkText")}>
        <Input className="w-80 font-mono text-xs" value={ndk} placeholder="~/Library/Android/sdk/ndk/30.0.16248370" onChange={(e) => setNdk(e.target.value)} onBlur={commit} />
      </Row>
    </Section>
  );
}
