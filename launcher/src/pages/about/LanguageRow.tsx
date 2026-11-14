import { useTranslation } from "react-i18next";
import { SelectRow } from "@/components/common";
import { useAction } from "@/hooks";
import { LAUNCHER_LANGUAGES } from "@/i18n";
import { api } from "@/lib";

const SYSTEM = "system";

/** The launcher's language: the system's, or one of the languages by its own name. */
export function LanguageRow({ language }: { language: string }) {
  const { t } = useTranslation();
  const save = useAction(api.saveLanguage);
  return (
    <SelectRow
      title={t("about.language")}
      value={language || SYSTEM}
      options={[{ value: SYSTEM, label: t("about.languageSystem") }, ...LAUNCHER_LANGUAGES.map((l) => ({ value: l.code as string, label: l.name }))]}
      onChange={(v) => save.mutate(v === SYSTEM ? "" : v)}
    />
  );
}
