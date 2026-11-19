import { useEffect, useState } from "react";

const QUERY = "(max-width: 767px)"; // Tailwind's max-md: the sidebar is an icon rail

/** Whether the window is narrow (a tablet held upright): the sidebar is then an icon rail, always shown. */
export function useNarrow() {
  const [narrow, setNarrow] = useState(() => typeof matchMedia !== "undefined" && matchMedia(QUERY).matches);
  useEffect(() => {
    const m = matchMedia(QUERY);
    const on = () => setNarrow(m.matches);
    m.addEventListener("change", on);
    return () => m.removeEventListener("change", on);
  }, []);
  return narrow;
}
