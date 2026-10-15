// Cross-platform gzip step for the PWA build output.
//
// Replaces the Unix-only `gzip` shell command in package.json so the
// build works in Windows PowerShell/cmd as well as bash. Every text asset
// in ./www is replaced by its `.gz` twin: the whole folder is copied into
// the robot's 2.6 MB FATFS "system" partition and the web server always
// looks for `<name>.gz` first, so keeping the plain copy next to it only
// wastes flash (m.js alone is 330 KB uncompressed). Already-compressed
// binaries (.woff2, .png) are left as they are.

import { readdirSync, readFileSync, writeFileSync, statSync, unlinkSync } from "node:fs";
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
  unlinkSync(file);
  console.log("gzip ->", name + ".gz");
  count++;
}
console.log(`gzipped ${count} file(s)`);
