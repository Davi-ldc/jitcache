// Each of the six documents in inputs/pdf, uploaded in name order.
import { readdirSync } from "node:fs";
import { join } from "node:path";

const directory = join(import.meta.dir, "../inputs/pdf");

export default await Promise.all(
  readdirSync(directory)
    .filter((name) => name.endsWith(".pdf"))
    .sort()
    .map(async (name) => ({
      method: "POST",
      path: "/",
      headers: { "content-type": "application/pdf" },
      body: await Bun.file(join(directory, name)).bytes(),
      status: 200,
    })),
);
