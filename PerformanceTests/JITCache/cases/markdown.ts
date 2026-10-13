// Content functions, such as docs sites, blogs and CMS previews: one pass renders 20 of Bun's documentation pages with
// marked and wraps each in an HTML layout with the site's navigation.

import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { Marked } from "marked";
import type { Workload } from "../run.ts";

interface Page {
  path: string; // relative to inputs/markdown, which mirrors the docs directory of Bun's repository
  href: string;
  title: string;
  description: string;
  body: string; // the page without its front matter
}

// The pages open with YAML front matter whose title and description are single lines.
function parsePage(path: string, source: string): Page {
  const match = /^---\n([\s\S]*?)\n---\n/.exec(source);
  const fields = new Map<string, string>();
  for (const line of match?.[1].split("\n") ?? []) {
    const field = /^(\w+):\s*(.*)$/.exec(line);
    if (field) fields.set(field[1], field[2].replace(/^(["'])(.*)\1$/, "$2"));
  }
  return {
    path,
    href: `/docs/${path.replace(/\.mdx?$/, "")}`,
    title: fields.get("title") ?? path,
    description: fields.get("description") ?? "",
    body: match ? source.slice(match[0].length) : source,
  };
}

const escapes: Record<string, string> = { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" };
const escapeHTML = (text: string) => text.replace(/[&<>"']/g, character => escapes[character]);

function layout(page: Page, content: string, pages: Page[]): string {
  const navigation = pages.map(other => `<li${other === page ? ' class="current"' : ""}><a href="${other.href}">${escapeHTML(other.title)}</a></li>`).join("");
  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>${escapeHTML(page.title)} | Bun docs</title>
<meta name="description" content="${escapeHTML(page.description)}">
<link rel="stylesheet" href="/docs.css">
</head>
<body>
<nav class="sidebar"><ul>${navigation}</ul></nav>
<main><article>
<h1>${escapeHTML(page.title)}</h1>
${content}
</article></main>
<footer><a href="https://github.com/oven-sh/bun/blob/main/docs/${page.path}">Edit this page</a></footer>
</body>
</html>
`;
}

export async function setup(): Promise<Workload> {
  const directory = join(import.meta.dir, "../inputs/markdown");
  const pages = readdirSync(directory, { recursive: true, encoding: "utf8" })
    .filter(path => /\.mdx?$/.test(path))
    .sort()
    .map(path => parsePage(path, readFileSync(join(directory, path), "utf8")));
  if (!pages.length) throw new Error(`${directory} holds no page`);
  const marked = new Marked({ gfm: true });
  return {
    pass() {
      return pages.map(page => layout(page, marked.parse(page.body, { async: false }), pages));
    },
  };
}
