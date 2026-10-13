// The most common serverless function: a JSON API behind a router, validation and middleware. A Hono app with Zod
// schemas serves an in-memory dataset through Bun.serve, and one pass sends it 100 requests with fetch from the same
// process, one at a time: user lookups, orders with valid and invalid bodies, searches that filter and sort, and
// health checks.

import { Hono } from "hono";
import { timing } from "hono/timing";
import { validator } from "hono/validator";
import { z } from "zod";
import type { Workload } from "../run.ts";

// mulberry32: every process draws the same dataset and the same requests.
function generator(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const firstNames = ["Ada", "Alan", "Barbara", "Claude", "Donald", "Edsger", "Frances", "Grace", "Hedy", "Ivan", "John", "Ken", "Leslie", "Margaret", "Niklaus", "Radia", "Shafi", "Tim", "Whitfield", "Yukihiro"];
const lastNames = ["Lovelace", "Turing", "Liskov", "Shannon", "Knuth", "Dijkstra", "Allen", "Hopper", "Lamarr", "Sutherland", "McCarthy", "Thompson", "Lamport", "Hamilton", "Wirth", "Perlman", "Goldwasser", "Berners-Lee", "Diffie", "Matsumoto"];
const countries = ["US", "CA", "GB", "DE", "FR", "BR", "JP", "IN", "AU", "MX"] as const;
const plans = ["free", "pro", "team"] as const;
const tags = ["beta", "newsletter", "enterprise", "churn-risk", "vip", "trial", "partner", "edu"];
const adjectives = ["Compact", "Classic", "Rugged", "Wireless", "Ergonomic", "Modular", "Portable", "Silent", "Smart", "Vintage"];
const nouns = ["Keyboard", "Lamp", "Backpack", "Kettle", "Monitor", "Chair", "Speaker", "Notebook", "Bottle", "Headset"];
const taxRates: Record<(typeof countries)[number], number> = { US: 0.07, CA: 0.13, GB: 0.2, DE: 0.19, FR: 0.2, BR: 0.17, JP: 0.1, IN: 0.18, AU: 0.1, MX: 0.16 };
const coupons: Record<string, number> = { WELCOME10: 10, SPRING25: 25, VIP40: 40 };
const searchTerms = ["ada", "lovelace", "us", "vip", "grace", "jp", "trial", "an", "berners", "edu"];
const searchSorts = ["name", "-name", "createdAt", "-createdAt", "spentCents", "-spentCents"] as const;
const userCount = 1000;
const productCount = 200;
const baseTime = Date.UTC(2025, 0, 1);
const day = 24 * 60 * 60 * 1000;

interface User {
  id: number;
  name: string;
  email: string;
  country: (typeof countries)[number];
  plan: (typeof plans)[number];
  createdAt: string;
  orderCount: number;
  spentCents: number;
  tags: string[];
}

interface Product {
  id: string;
  name: string;
  priceCents: number;
  stock: number;
}

const idSchema = z.object({ id: z.coerce.number().int().positive() });

const orderSchema = z.object({
  userId: z.number().int().positive(),
  items: z
    .array(z.object({ productId: z.string().regex(/^p\d{3}$/), quantity: z.number().int().min(1).max(20) }))
    .min(1)
    .max(10),
  shipping: z.object({
    country: z.enum(countries),
    postalCode: z.string().regex(/^[A-Z0-9][A-Z0-9 -]{1,8}[A-Z0-9]$/),
    express: z.boolean().default(false),
  }),
  coupon: z
    .string()
    .regex(/^[A-Z0-9]{4,12}$/)
    .optional(),
  note: z.string().max(140).optional(),
});

const searchSchema = z.object({
  q: z.string().trim().min(1).max(40),
  sort: z.enum(searchSorts).default("name"),
  limit: z.coerce.number().int().min(1).max(50).default(20),
});

// Zod's issues, reduced to what a client acts on.
function issuesOf(error: z.ZodError) {
  return error.issues.map(issue => ({ path: issue.path.join("."), code: issue.code, message: issue.message }));
}

function buildApp(users: Map<number, User>, products: Map<string, Product>) {
  const userList = [...users.values()];
  const orders = new Map<string, object>();
  let orderSequence = 0;
  const app = new Hono();

  // Server-Timing carries the handler's duration, which differs on every request, so the checksum leaves headers out.
  app.use(timing());

  app.get(
    "/users/:id",
    validator("param", (value, c) => {
      const parsed = idSchema.safeParse(value);
      return parsed.success ? parsed.data : c.json({ error: "invalid user id", issues: issuesOf(parsed.error) }, 400);
    }),
    c => {
      const { id } = c.req.valid("param");
      const user = users.get(id);
      if (!user) return c.json({ error: "user not found", id }, 404);
      return c.json(user);
    },
  );

  app.post(
    "/orders",
    validator("json", (value, c) => {
      const parsed = orderSchema.safeParse(value);
      return parsed.success ? parsed.data : c.json({ error: "invalid order", issues: issuesOf(parsed.error) }, 400);
    }),
    c => {
      const input = c.req.valid("json");
      if (!users.has(input.userId)) return c.json({ error: "unknown user", userId: input.userId }, 422);
      const lines = [];
      for (const item of input.items) {
        const product = products.get(item.productId);
        if (!product) return c.json({ error: "unknown product", productId: item.productId }, 422);
        lines.push({ productId: product.id, name: product.name, quantity: item.quantity, unitCents: product.priceCents, totalCents: product.priceCents * item.quantity });
      }
      const subtotalCents = lines.reduce((sum, line) => sum + line.totalCents, 0);
      const discountCents = input.coupon && coupons[input.coupon] ? Math.floor((subtotalCents * coupons[input.coupon]) / 100) : 0;
      const shippingCents = input.shipping.express ? 1500 : subtotalCents - discountCents >= 5000 ? 0 : 499;
      const taxCents = Math.round((subtotalCents - discountCents) * taxRates[input.shipping.country]);
      const sequence = ++orderSequence;
      const order = {
        id: `ord_${String(sequence).padStart(6, "0")}`,
        userId: input.userId,
        status: "created",
        items: lines,
        shipping: input.shipping,
        coupon: input.coupon ?? null,
        note: input.note ?? null,
        subtotalCents,
        discountCents,
        shippingCents,
        taxCents,
        totalCents: subtotalCents - discountCents + shippingCents + taxCents,
        currency: "USD",
        // A clock reading would differ between processes; the order's time follows its sequence number.
        createdAt: new Date(baseTime + sequence * 60_000).toISOString(),
      };
      orders.set(order.id, order);
      c.header("Location", `/orders/${order.id}`);
      return c.json(order, 201);
    },
  );

  app.get(
    "/search",
    validator("query", (value, c) => {
      const parsed = searchSchema.safeParse(value);
      return parsed.success ? parsed.data : c.json({ error: "invalid search", issues: issuesOf(parsed.error) }, 400);
    }),
    c => {
      const { q, sort, limit } = c.req.valid("query");
      const needle = q.toLowerCase();
      const matches = userList.filter(user => user.name.toLowerCase().includes(needle) || user.email.includes(needle) || user.country.toLowerCase() === needle || user.tags.includes(needle));
      const descending = sort.startsWith("-");
      const field = (descending ? sort.slice(1) : sort) as "name" | "createdAt" | "spentCents";
      const direction = descending ? -1 : 1;
      matches.sort((a, b) => (a[field] < b[field] ? -direction : a[field] > b[field] ? direction : a.id - b.id));
      const results = matches.slice(0, limit).map(({ id, name, email, country, plan, spentCents }) => ({ id, name, email, country, plan, spentCents }));
      return c.json({ query: q, sort, total: matches.length, results });
    },
  );

  app.get("/health", c => c.json({ status: "ok", users: users.size, products: products.size, orders: orders.size }));

  app.notFound(c => c.json({ error: "not found" }, 404));
  app.onError((error, c) => c.json({ error: "internal error", message: error.message }, 500));
  return app;
}

interface Call {
  method: "GET" | "POST";
  path: string;
  body?: string;
  status: number; // the status the call must get
}

function buildCalls(random: () => number, users: Map<number, User>, products: Map<string, Product>): Call[] {
  const pick = <T>(list: readonly T[]) => list[Math.floor(random() * list.length)];
  const productIds = [...products.keys()];
  const validOrder = () => ({
    userId: 1 + Math.floor(random() * users.size),
    items: Array.from({ length: 1 + Math.floor(random() * 4) }, () => ({ productId: pick(productIds), quantity: 1 + Math.floor(random() * 3) })),
    shipping: { country: pick(countries), postalCode: `${10000 + Math.floor(random() * 89999)}`, express: random() < 0.3 },
    ...(random() < 0.4 ? { coupon: pick(Object.keys(coupons)) } : {}),
    ...(random() < 0.3 ? { note: "Leave the parcel at the front desk." } : {}),
  });
  // Every fifth order is invalid, each in its own way, so a pass gets six 400 answers that carry Zod's issues.
  const invalidOrders = [
    { ...validOrder(), userId: 0 },
    { ...validOrder(), items: [] },
    { ...validOrder(), items: [{ productId: "p007", quantity: 0 }] },
    { ...validOrder(), shipping: { country: "XX", postalCode: "12345" } },
    { ...validOrder(), shipping: { country: "US", postalCode: "!" } },
    { ...validOrder(), userId: "42", note: "x".repeat(200) },
  ];

  const calls: Call[] = [];
  for (let i = 0; i < 40; ++i) calls.push({ method: "GET", path: `/users/${1 + Math.floor(random() * users.size)}`, status: 200 });
  for (let i = 0; i < 30; ++i) {
    const invalid = i % 5 === 4;
    const body = invalid ? invalidOrders[(i - 4) / 5] : validOrder();
    calls.push({ method: "POST", path: "/orders", body: JSON.stringify(body), status: invalid ? 400 : 201 });
  }
  for (let i = 0; i < 20; ++i) {
    const limit = i % 3 === 0 ? `&limit=${5 + i}` : "";
    calls.push({ method: "GET", path: `/search?q=${encodeURIComponent(searchTerms[i % searchTerms.length])}&sort=${searchSorts[i % searchSorts.length]}${limit}`, status: 200 });
  }
  for (let i = 0; i < 10; ++i) calls.push({ method: "GET", path: "/health", status: 200 });

  // A burst mixes the kinds of request; a seeded shuffle fixes one mix for every pass and every process.
  for (let i = calls.length - 1; i > 0; --i) {
    const j = Math.floor(random() * (i + 1));
    [calls[i], calls[j]] = [calls[j], calls[i]];
  }
  return calls;
}

export async function setup(): Promise<Workload> {
  const random = generator(0x5eed);
  const users = new Map<number, User>();
  for (let id = 1; id <= userCount; ++id) {
    const first = firstNames[Math.floor(random() * firstNames.length)];
    const last = lastNames[Math.floor(random() * lastNames.length)];
    users.set(id, {
      id,
      name: `${first} ${last}`,
      email: `${first}.${last}.${id}@example.com`.toLowerCase(),
      country: countries[Math.floor(random() * countries.length)],
      plan: plans[Math.floor(random() * plans.length)],
      createdAt: new Date(baseTime - Math.floor(random() * 900) * day).toISOString(),
      orderCount: Math.floor(random() * 60),
      spentCents: Math.floor(random() * 500_000),
      tags: tags.filter(() => random() < 0.15),
    });
  }
  const products = new Map<string, Product>();
  for (let i = 1; i <= productCount; ++i) {
    const id = `p${String(i).padStart(3, "0")}`;
    products.set(id, { id, name: `${adjectives[i % adjectives.length]} ${nouns[Math.floor(i / adjectives.length) % nouns.length]}`, priceCents: 500 + Math.floor(random() * 20_000), stock: Math.floor(random() * 50) });
  }

  const calls = buildCalls(random, users, products);
  const server = Bun.serve({ port: 0, hostname: "127.0.0.1", fetch: buildApp(users, products).fetch });
  const origin = `http://127.0.0.1:${server.port}`;
  const jsonHeaders = { "content-type": "application/json" };

  return {
    async pass() {
      const output: string[] = [];
      for (const call of calls) {
        const response = await fetch(origin + call.path, call.body === undefined ? undefined : { method: call.method, headers: jsonHeaders, body: call.body });
        const body = await response.text();
        if (response.status !== call.status) throw new Error(`${call.method} ${call.path} answered ${response.status} where ${call.status} was due: ${body.slice(0, 400)}`);
        if (!response.headers.has("server-timing")) throw new Error(`${call.method} ${call.path} answered without the timing middleware's header`);
        output.push(`${response.status} ${body}`);
      }
      return output;
    },
    async close() {
      await server.stop(true);
    },
  };
}
