import { useQuery } from "@tanstack/react-query";
import { api } from "@/lib";

/** A game's card art (data: URL), or null: fetched once a session. */
export function useGameArt(id: string) {
  return useQuery({ queryKey: ["art", id], queryFn: () => api.gameArt(id), staleTime: Infinity, gcTime: Infinity, retry: false });
}
