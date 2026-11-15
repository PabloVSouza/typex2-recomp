import { useQuery } from "@tanstack/react-query";
import { useAppState } from "@/hooks/useAppState";
import { api } from "@/lib";

const SIX_HOURS = 6 * 60 * 60 * 1000;

/** The newest release on GitHub, checked at start and every six hours while the setting is on
 *  (About can check by hand either way). A failed check never gets in the way of the launcher. */
export function useRelease() {
  const { data } = useAppState();
  const automatic = !!data?.check_updates;
  return useQuery({
    queryKey: ["release"],
    queryFn: api.checkRelease,
    enabled: automatic,
    staleTime: SIX_HOURS,
    refetchInterval: automatic ? SIX_HOURS : false,
    retry: 1,
  });
}
