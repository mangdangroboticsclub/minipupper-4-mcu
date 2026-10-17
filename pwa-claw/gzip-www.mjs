// Cross-platform gzip step for the PWA build output.
//
// Replaces the Unix-only `gzip` shell command in package.json so the
// build works in Windows PowerShell/cmd as well as bash. Produces a
// `.gz` next to every embeddable asset in ./www (the firmware embeds
// these gzipped variants via EMBED_FILES in main/CMakeLists.txt).

import { readdirSync, readFileSync, writeFileSync, statSync } from "node:fs";
import { gzipSync } from "node:zlib";
import { fileURLToPath } from "node:url";
import { join, dirname } from "node:path";

const wwwDir = join(dirname(fileURLToPath(import.meta.url)), "www");
const exts = [".html", ".js", ".css", ".svg", ".json"];

let count = 0;
for (const name of readdirSync(wwwDir)) {
  if (name.endsWith(".gz")) continue;
  if (!exts.some((e) => name.endsWith(e))) continue;

  const file = join(wwwDir, name);
  if (!statSync(file).isFile()) continue;

  writeFileSync(file + ".gz", gzipSync(readFileSync(file), { level: 9 }));
  console.log("gzip ->", name + ".gz");
  count++;
}
console.log(`gzipped ${count} file(s)`);
