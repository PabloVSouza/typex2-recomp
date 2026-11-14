import { useQuery } from "@tanstack/react-query";
import { api } from "@/lib";

/** Everything the launcher shows: the library, settings, tools, exports, the running job. */
export function useAppState() {
  return useQuery({ queryKey: ["state"], queryFn: api.loadState });
}
