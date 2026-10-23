// The results file of one invocation of the runner, one JSON object per line, under the build root, or in the
// invocation's temporary directory when the build root takes no file. A bench's file names start with "bench-" and a
// ladder's with "ladder-".

import { appendFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";

export class ResultsLog {
  readonly path: string;

  constructor(directory: string, fallback: string, prefix = "") {
    const name = `${prefix}${new Date().toISOString().replace(/[:.]/g, "-")}-${process.pid}.jsonl`;
    try {
      mkdirSync(directory, { recursive: true });
      this.path = join(directory, name);
      appendFileSync(this.path, "");
    } catch {
      this.path = join(fallback, name);
    }
  }

  write(record: object) {
    appendFileSync(this.path, `${JSON.stringify(record)}\n`);
  }
}
