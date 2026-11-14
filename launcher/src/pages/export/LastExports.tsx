import { Boxes, FolderOpen } from "lucide-react";
import { useTranslation } from "react-i18next";
import { Empty, Row, Section, SectionHeading } from "@/components/common";
import { Badge, Button } from "@/components/ui";
import { reveal } from "@/lib";
import { targetName } from "@/lib/data";
import { when } from "@/lib";
import type { AppStateData } from "@/lib";

export function LastExports({ data }: { data: AppStateData }) {
  const { t } = useTranslation();
  return (
    <>
      <SectionHeading>{t("export.last")}</SectionHeading>
      {data.exports.length === 0 ? (
        <Empty icon={<Boxes />} title={t("export.none")} />
      ) : (
        <Section>
          {data.exports.map((e) => {
            const target = data.targets.find((x) => x.id === e.target);
            return (
              <Row
                key={e.target}
                title={target ? targetName(t, target) : e.target}
                description={`${e.games.map((id) => data.catalog.find((g) => g.id === id)?.title ?? id).join(", ")} · ${when(e.built)}`}
              >
                <Badge variant="secondary">{e.files[0]?.split(/[\\/]/).pop()}</Badge>
                <Button variant="secondary" size="sm" onClick={() => e.files[0] && reveal(e.files[0])}><FolderOpen /> {t("common.show")}</Button>
              </Row>
            );
          })}
        </Section>
      )}
    </>
  );
}
