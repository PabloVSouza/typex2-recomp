import { useEffect } from "react";
import { useAppState } from "@/hooks/useAppState";
import { i18n, resolveLanguage } from "@/i18n";

/** Applies the launcher language of the settings ("" = the system's). */
export function useLanguageSync() {
  const { data } = useAppState();
  const wanted = data?.language;
  useEffect(() => {
    if (wanted === undefined) return;
    const language = resolveLanguage(wanted);
    if (i18n.language !== language) i18n.changeLanguage(language);
    document.documentElement.lang = language;
  }, [wanted]);
}
