import { useTranslation } from "react-i18next";
import { PageHeader, Row, Section } from "@/components/common";
import { useAppState } from "@/hooks";
import { LanguageRow } from "@/pages/about/LanguageRow";

export function AboutPage() {
  const { t } = useTranslation();
  const { data } = useAppState();
  if (!data) return null;
  const kit = data.kit ? `${data.kit.kind === "bundled" ? t("about.kitBundled") : t("about.kitCheckout")}: ${data.kit.source}` : t("about.kitMissing");
  return (
    <>
      <PageHeader kicker={t("nav.about")} title="Type X Recomp" description={t("about.description")} />
      <Section title={t("about.settings")}>
        <LanguageRow language={data.language} />
      </Section>
      <Section title={t("about.noFiles")}>
        <Row title={t("about.yours")} description={t("about.yoursText")} />
      </Section>
      <Section title={t("about.installation")}>
        <Row title={t("about.launcher")} description={`${data.version} · ${data.os} ${data.arch}`} />
        <Row title={t("about.kit")} description={kit} />
        <Row title={t("about.data")} description={<span className="break-all font-mono">{data.data}</span>} />
      </Section>
    </>
  );
}
