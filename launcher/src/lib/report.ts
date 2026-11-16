import type { AppStateData } from "@/lib/types";

export const ISSUES_NEW = "https://github.com/PabloVSouza/typex2-recomp/issues/new";
const TAIL = 200;

/** The user's home folder in a path becomes ~ (it usually holds their name). */
export function anonymize(text: string): string {
  return text.replace(/(\/Users\/|\/home\/)[^/\s]+/g, "~").replace(/[A-Za-z]:\\Users\\[^\\\s]+/g, "~");
}

/** A problem report: the installation, the games and their builds, the end of both logs. */
export function buildReport(data: AppStateData, gameLog: string[], buildLog: string[]): string {
  const games = data.games.map((g) => `- ${g.title}: ${!g.supported ? "not supported yet" : g.found ? g.library.state : "folder missing"}`);
  const tail = (lines: string[]) => (lines.length ? lines.slice(-TAIL).join("\n") : "(empty)");
  const text = [
    "### What happened",
    "",
    "(Describe the problem: what you did, what you expected, what happened.)",
    "",
    "### Installation",
    "",
    `- Launcher ${data.version} on ${data.os} ${data.arch}`,
    `- Kit: ${data.kit ? `${data.kit.kind}` : "missing"}`,
    "",
    "### Games",
    "",
    ...(games.length ? games : ["(none)"]),
    "",
    `<details><summary>Game output (last ${TAIL} lines)</summary>`,
    "",
    "```",
    tail(gameLog),
    "```",
    "</details>",
    "",
    `<details><summary>Build output (last ${TAIL} lines)</summary>`,
    "",
    "```",
    tail(buildLog),
    "```",
    "</details>",
  ].join("\n");
  return anonymize(text);
}
