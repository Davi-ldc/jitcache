import { Hono } from "hono";
import { zValidator } from "@hono/zod-validator";
import { z } from "zod";
import posts from "./posts.json";

const postSchema = z.object({
  title: z.string().min(1).max(100),
  author: z.string().min(1),
  tags: z.array(z.string()).max(5).default([]),
  body: z.string().min(1),
});

let nextId = posts.length + 1;

const app = new Hono();

app.get("/", (c) => c.text("Hello Hono!"));

app.get("/posts", (c) => {
  const tag = c.req.query("tag");
  return c.json(tag ? posts.filter((post) => post.tags.includes(tag)) : posts);
});

app.get("/posts/:id", (c) => {
  const post = posts.find((post) => post.id === Number(c.req.param("id")));
  if (!post) return c.json({ error: "Post not found" }, 404);
  return c.json(post);
});

app.post("/posts", zValidator("json", postSchema), (c) => {
  const post = { id: nextId++, ...c.req.valid("json") };
  posts.push(post);
  return c.json(post, 201);
});

app.delete("/posts/:id", (c) => {
  const index = posts.findIndex((post) => post.id === Number(c.req.param("id")));
  if (index === -1) return c.json({ error: "Post not found" }, 404);
  posts.splice(index, 1);
  return c.body(null, 204);
});

export default app;
