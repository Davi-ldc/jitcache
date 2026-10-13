// A document-ingestion function, as RAG pipelines and back-office tools deploy it, served by Bun.serve with two
// endpoints. POST /documents takes a PDF, extracts its text page by page with pdf.js (unpdf's serverless build, which
// runs pdf.js's worker code on the calling thread), normalizes whitespace, splits each page into chunks of about 1000
// characters that overlap by about 100, hashes each chunk with SHA-256 and answers the chunks with the document's
// metadata. POST /events takes a webhook batch of orders, refunds and customer updates, validates every record with
// strict Zod schemas, reports the invalid ones, skips redelivered ones, normalizes the rest and answers totals by
// currency and type and a summary per customer. One pass sends every PDF of inputs/pdf once and three event batches,
// with fetch from the same process, one request at a time.

import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { extractText, getDocumentProxy, getMeta, getResolvedPDFJS } from "unpdf";
import { z } from "zod";
import type { Workload } from "../run.ts";

// mulberry32: every process builds the same event batches.
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

const json = (status: number, body: unknown) => Response.json(body, { status });
const problem = (code: string, message: string) => ({ error: { code, message } });

async function sha256(data: Uint8Array): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", data));
  let hex = "";
  for (const byte of digest) hex += byte.toString(16).padStart(2, "0");
  return hex;
}

// ---------------------------------------------------------------------------------------------------------------------
// POST /documents

const maxDocumentBytes = 20 * 1024 * 1024;
const chunkSize = 1000;
const chunkOverlap = 100;
const encoder = new TextEncoder();

// NFKC folds the ligatures and compatibility forms PDFs carry (the ligature U+FB01 becomes "fi"), soft hyphens only
// mark where a word may break, and the line breaks of extracted text follow the page's layout, not its sentences.
function normalize(text: string): string {
  return text.normalize("NFKC").replaceAll("\xad", "").replace(/\s+/g, " ").trim();
}

// Chunks of about chunkSize characters that end at a space, unless a word is longer than half a chunk, and start at a
// word about chunkOverlap characters before the previous chunk's end, so a sentence cut by one chunk is whole in the
// next.
function split(text: string): string[] {
  const chunks: string[] = [];
  let start = 0;
  while (start < text.length) {
    let end = Math.min(start + chunkSize, text.length);
    if (end < text.length) {
      const space = text.lastIndexOf(" ", end);
      if (space > start + chunkSize / 2) end = space;
    }
    chunks.push(text.slice(start, end));
    if (end === text.length) break;
    let next = end - chunkOverlap;
    const space = text.indexOf(" ", next);
    if (space !== -1 && space < end) next = space + 1;
    start = next;
  }
  return chunks;
}

async function ingestDocument(request: Request): Promise<Response> {
  const name = new URL(request.url).searchParams.get("name") ?? "";
  if (!/^[A-Za-z0-9][\w.-]{0,127}\.pdf$/.test(name)) return json(400, problem("invalid_name", "name must be a file name that ends in .pdf"));
  if (request.headers.get("content-type") !== "application/pdf") return json(415, problem("unsupported_media_type", "the body must be application/pdf"));
  const bytes = new Uint8Array(await request.arrayBuffer());
  if (bytes.length === 0 || bytes.length > maxDocumentBytes) return json(413, problem("size", `a document takes 1 byte to ${maxDocumentBytes} bytes`));
  const size = bytes.length;
  // pdf.js takes the buffer it parses and detaches it, so the document's hash comes first.
  const id = await sha256(bytes);
  let pdf: Awaited<ReturnType<typeof getDocumentProxy>>;
  try {
    // Verbosity 0 keeps pdf.js's warnings about fonts, which it prints with console.log, out of the function's output.
    pdf = await getDocumentProxy(bytes, { verbosity: 0 });
  } catch (error) {
    return json(422, problem("unreadable_pdf", (error as Error).message));
  }
  try {
    const { totalPages, text } = await extractText(pdf);
    const { info } = await getMeta(pdf, { parseDates: true });
    const chunks: { id: string; page: number; index: number; characters: number; sha256: string; text: string }[] = [];
    for (const [pageIndex, pageText] of text.entries()) {
      const pieces = split(normalize(pageText));
      const hashes = await Promise.all(pieces.map(piece => sha256(encoder.encode(piece))));
      pieces.forEach((piece, index) => chunks.push({ id: `${id.slice(0, 16)}-p${pageIndex + 1}-c${index}`, page: pageIndex + 1, index, characters: piece.length, sha256: hashes[index], text: piece }));
    }
    return json(200, { document: { id, name, bytes: size, pages: totalPages, metadata: info }, chunks });
  } finally {
    await pdf.loadingTask.destroy();
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// POST /events

// ISO 4217's minor units: amounts arrive as decimal strings and are summed as integers in the minor unit.
const exponents = { USD: 2, CAD: 2, GBP: 2, EUR: 2, BRL: 2, JPY: 0 } as const;
type Currency = keyof typeof exponents;
const currencyCodes = Object.keys(exponents) as [Currency, ...Currency[]];
const eventTypes = ["order.created", "order.refunded", "customer.updated"] as const;

const idOf = (prefix: string, length: number) => z.string().regex(new RegExp(`^${prefix}_[a-z0-9]{${length}}$`));
const timestamp = z.iso.datetime({ offset: true });
const money = z
  .strictObject({ amount: z.string().regex(/^\d{1,9}(\.\d{1,3})?$/), currency: z.enum(currencyCodes) })
  .refine(value => (value.amount.split(".")[1]?.length ?? 0) <= exponents[value.currency], { message: "the amount has more decimals than its currency", path: ["amount"] });
const address = z.strictObject({
  line1: z.string().min(1).max(200),
  line2: z.string().max(200).optional(),
  city: z.string().min(1).max(100),
  region: z.string().max(100).optional(),
  postalCode: z.string().regex(/^[A-Z0-9][A-Z0-9 -]{1,8}[A-Z0-9]$/),
  country: z.string().regex(/^[A-Z]{2}$/),
});
const customerReference = z.strictObject({ id: idOf("cus", 10), email: z.email() });
const sku = z.string().regex(/^[A-Z]{3}-\d{4}$/);

function toMinor(value: { amount: string; currency: Currency }): number {
  const [whole, fraction = ""] = value.amount.split(".");
  const exponent = exponents[value.currency];
  return Number(whole) * 10 ** exponent + Number(fraction.padEnd(exponent, "0") || 0);
}

function formatMinor(minor: number, currency: Currency): string {
  const exponent = exponents[currency];
  const sign = minor < 0 ? "-" : "";
  const absolute = Math.abs(minor);
  if (!exponent) return `${sign}${absolute}`;
  return `${sign}${Math.floor(absolute / 10 ** exponent)}.${String(absolute % 10 ** exponent).padStart(exponent, "0")}`;
}

const orderCreated = z.strictObject({
  id: idOf("evt", 16),
  type: z.literal("order.created"),
  occurredAt: timestamp,
  data: z
    .strictObject({
      orderId: idOf("ord", 10),
      customer: customerReference,
      lines: z.array(z.strictObject({ sku, name: z.string().min(1).max(120), quantity: z.number().int().min(1).max(999), unitPrice: money })).min(1).max(50),
      shipping: z.strictObject({ method: z.enum(["standard", "express", "pickup"]), address: address.nullable(), price: money }),
      discounts: z.array(z.strictObject({ code: z.string().regex(/^[A-Z0-9]{4,16}$/), amount: money })).max(5),
      total: money,
    })
    // A total that disagrees with its lines is a bug in the sender, and the order must not be counted.
    .superRefine((order, context) => {
      const currency = order.total.currency;
      const amounts = [...order.lines.map(line => line.unitPrice), order.shipping.price, ...order.discounts.map(discount => discount.amount)];
      if (amounts.some(amount => amount.currency !== currency)) {
        context.addIssue({ code: "custom", message: `every amount must be in ${currency}`, path: ["total", "currency"] });
        return;
      }
      const expected = order.lines.reduce((sum, line) => sum + toMinor(line.unitPrice) * line.quantity, 0) + toMinor(order.shipping.price) - order.discounts.reduce((sum, discount) => sum + toMinor(discount.amount), 0);
      if (toMinor(order.total) !== expected) context.addIssue({ code: "custom", message: `the total must be ${formatMinor(expected, currency)}`, path: ["total", "amount"] });
    }),
});

const orderRefunded = z.strictObject({
  id: idOf("evt", 16),
  type: z.literal("order.refunded"),
  occurredAt: timestamp,
  data: z.strictObject({
    refundId: idOf("re", 12),
    orderId: idOf("ord", 10),
    customer: customerReference,
    amount: money,
    reason: z.enum(["requested_by_customer", "duplicate", "fraudulent", "damaged"]),
    lines: z.array(z.strictObject({ sku, quantity: z.number().int().min(1).max(999) })).optional(),
  }),
});

const customerUpdated = z.strictObject({
  id: idOf("evt", 16),
  type: z.literal("customer.updated"),
  occurredAt: timestamp,
  data: z.strictObject({
    customer: z.strictObject({
      id: idOf("cus", 10),
      email: z.email(),
      name: z.string().min(1).max(120),
      phone: z
        .string()
        .regex(/^\+[1-9]\d{6,14}$/)
        .nullable(),
      address: address.nullable(),
      marketingOptIn: z.boolean(),
      tags: z.array(z.string().regex(/^[a-z0-9-]{1,32}$/)).max(20),
    }),
    changed: z.array(z.enum(["email", "name", "phone", "address", "marketingOptIn", "tags"])).min(1),
  }),
});

const eventSchema = z.discriminatedUnion("type", [orderCreated, orderRefunded, customerUpdated]);
const batchSchema = z.strictObject({
  batchId: idOf("batch", 12),
  source: z.enum(["storefront", "pos", "marketplace"]),
  sentAt: timestamp,
  // Each event is validated on its own, so one bad record is reported without failing the batch.
  events: z.array(z.unknown()).min(1).max(1000),
});

const issuesOf = (error: z.ZodError) => error.issues.map(issue => ({ path: issue.path.join("."), code: issue.code, message: issue.message }));
const field = (value: unknown, name: string) => (value && typeof value === "object" && typeof (value as Record<string, unknown>)[name] === "string" ? (value as Record<string, string>)[name] : null);

interface CustomerSummary {
  id: string;
  email: string;
  name: string | null;
  country: string | null;
  marketingOptIn: boolean | null;
  orders: number;
  items: number;
  refunds: number;
  spent: Map<Currency, number>;
  refunded: Map<Currency, number>;
  firstSeen: string;
  lastSeen: string;
}

async function ingestEvents(request: Request): Promise<Response> {
  let payload: unknown;
  try {
    payload = await request.json();
  } catch {
    return json(400, problem("malformed_json", "the body is not valid JSON"));
  }
  const batch = batchSchema.safeParse(payload);
  if (!batch.success) return json(422, { error: { code: "invalid_batch", message: "the batch envelope is invalid", issues: issuesOf(batch.error) } });

  const rejected: { index: number; id: string | null; type: string | null; issues: ReturnType<typeof issuesOf> }[] = [];
  const seen = new Set<string>();
  const byType = Object.fromEntries(eventTypes.map(type => [type, 0])) as Record<(typeof eventTypes)[number], number>;
  let duplicates = 0;
  const totals = new Map<Currency, { orders: number; ordered: number; refunds: number; refunded: number }>();
  const customers = new Map<string, CustomerSummary>();

  const customerOf = (id: string, email: string, occurredAt: string) => {
    let customer = customers.get(id);
    if (!customer) customers.set(id, (customer = { id, email, name: null, country: null, marketingOptIn: null, orders: 0, items: 0, refunds: 0, spent: new Map(), refunded: new Map(), firstSeen: occurredAt, lastSeen: occurredAt }));
    // Events arrive out of order, so the first and last times come from comparing, and the email from the latest event.
    if (occurredAt < customer.firstSeen) customer.firstSeen = occurredAt;
    if (occurredAt >= customer.lastSeen) {
      customer.lastSeen = occurredAt;
      customer.email = email;
    }
    return customer;
  };
  const totalOf = (currency: Currency) => {
    let total = totals.get(currency);
    if (!total) totals.set(currency, (total = { orders: 0, ordered: 0, refunds: 0, refunded: 0 }));
    return total;
  };
  const add = (map: Map<Currency, number>, currency: Currency, minor: number) => map.set(currency, (map.get(currency) ?? 0) + minor);

  batch.data.events.forEach((raw, index) => {
    const parsed = eventSchema.safeParse(raw);
    if (!parsed.success) {
      rejected.push({ index, id: field(raw, "id"), type: field(raw, "type"), issues: issuesOf(parsed.error) });
      return;
    }
    const event = parsed.data;
    // Webhooks are delivered at least once; a redelivered event counts once.
    if (seen.has(event.id)) {
      ++duplicates;
      return;
    }
    seen.add(event.id);
    ++byType[event.type];
    const occurredAt = new Date(event.occurredAt).toISOString();
    switch (event.type) {
      case "order.created": {
        const { customer, lines, total, shipping } = event.data;
        const summary = customerOf(customer.id, customer.email.trim().toLowerCase(), occurredAt);
        const minor = toMinor(total);
        ++summary.orders;
        summary.items += lines.reduce((sum, line) => sum + line.quantity, 0);
        add(summary.spent, total.currency, minor);
        summary.country ??= shipping.address?.country ?? null;
        const totalForCurrency = totalOf(total.currency);
        ++totalForCurrency.orders;
        totalForCurrency.ordered += minor;
        break;
      }
      case "order.refunded": {
        const { customer, amount } = event.data;
        const summary = customerOf(customer.id, customer.email.trim().toLowerCase(), occurredAt);
        const minor = toMinor(amount);
        ++summary.refunds;
        add(summary.refunded, amount.currency, minor);
        const totalForCurrency = totalOf(amount.currency);
        ++totalForCurrency.refunds;
        totalForCurrency.refunded += minor;
        break;
      }
      case "customer.updated": {
        const { customer } = event.data;
        const summary = customerOf(customer.id, customer.email.trim().toLowerCase(), occurredAt);
        if (summary.lastSeen === occurredAt) {
          summary.name = customer.name;
          summary.marketingOptIn = customer.marketingOptIn;
          if (customer.address) summary.country = customer.address.country;
        }
        break;
      }
    }
  });

  const amounts = (map: Map<Currency, number>) => Object.fromEntries([...map].sort(([a], [b]) => (a < b ? -1 : 1)).map(([currency, minor]) => [currency, formatMinor(minor, currency)]));
  return json(200, {
    batchId: batch.data.batchId,
    source: batch.data.source,
    sentAt: new Date(batch.data.sentAt).toISOString(),
    counts: { received: batch.data.events.length, accepted: seen.size, duplicates, rejected: rejected.length, byType },
    totals: [...totals]
      .sort(([a], [b]) => (a < b ? -1 : 1))
      .map(([currency, total]) => ({
        currency,
        orders: { count: total.orders, amount: formatMinor(total.ordered, currency) },
        refunds: { count: total.refunds, amount: formatMinor(total.refunded, currency) },
        net: formatMinor(total.ordered - total.refunded, currency),
      })),
    customers: [...customers.values()]
      .sort((a, b) => (a.id < b.id ? -1 : 1))
      .map(customer => ({ ...customer, spent: amounts(customer.spent), refunded: amounts(customer.refunded) })),
    rejected,
  });
}

// ---------------------------------------------------------------------------------------------------------------------
// The event batches, built at setup

const regionsByCountry = {
  US: { currency: "USD", offsets: [-300, -480], cities: [["Portland", "97205"], ["Austin", "78701"], ["Boston", "02110"]] },
  CA: { currency: "CAD", offsets: [-300], cities: [["Toronto", "M5V 2T6"], ["Montreal", "H2Y 1C6"]] },
  GB: { currency: "GBP", offsets: [0, 60], cities: [["London", "SW1A 1AA"], ["Leeds", "LS1 4AP"]] },
  DE: { currency: "EUR", offsets: [60], cities: [["Berlin", "10115"], ["Hamburg", "20095"]] },
  FR: { currency: "EUR", offsets: [60], cities: [["Paris", "75008"], ["Lyon", "69002"]] },
  BR: { currency: "BRL", offsets: [-180], cities: [["Sao Paulo", "01310-100"], ["Curitiba", "80010-000"]] },
  JP: { currency: "JPY", offsets: [540], cities: [["Tokyo", "100-0001"], ["Osaka", "530-0001"]] },
} as const;
type Country = keyof typeof regionsByCountry;
// Fixed rates, so a product's price in each currency is the same in every batch.
const rates: Record<Currency, number> = { USD: 1, CAD: 1.37, GBP: 0.79, EUR: 0.92, BRL: 5.4, JPY: 150 };
const productNames = ["Trail Runner", "Rain Shell", "Day Pack", "Camp Mug", "Wool Socks", "Headlamp", "Water Filter", "Tent Stakes", "Fleece Vest", "Dry Bag"];
const givenNames = ["Ana", "Bruno", "Chloe", "Daniel", "Emma", "Felix", "Giulia", "Hiro", "Ines", "Jonas", "Kenji", "Lea", "Mateo", "Nora", "Olga", "Pedro"];
const familyNames = ["Silva", "Martin", "Schmidt", "Tanaka", "Dubois", "Smith", "Rossi", "Santos", "Muller", "Sato", "Bernard", "Brown"];
const batchSizes = [300, 250, 350];
const faultsPerBatch = 4;
const redeliveriesPerBatch = 3;

interface Customer {
  id: string;
  name: string;
  email: string;
  phone: string;
  country: Country;
  currency: Currency;
  address: { line1: string; city: string; postalCode: string; country: string };
}

interface Batch {
  body: string;
  events: number;
  invalid: number[]; // the indexes of the records that must be rejected
  redeliveries: number;
}

function buildBatches(random: () => number): Batch[] {
  const pick = <T>(list: readonly T[]) => list[Math.floor(random() * list.length)];
  const token = (length: number) => Array.from({ length }, () => "0123456789abcdefghijklmnopqrstuvwxyz"[Math.floor(random() * 36)]).join("");
  const money = (minor: number, currency: Currency) => ({ amount: formatMinor(minor, currency), currency });
  const countries = Object.keys(regionsByCountry) as Country[];

  const customers: Customer[] = Array.from({ length: 120 }, (_, i) => {
    const country = pick(countries);
    const region = regionsByCountry[country];
    const [city, postalCode] = pick(region.cities);
    const given = pick(givenNames);
    const family = pick(familyNames);
    // Mixed case as customers type them; the function lowercases emails.
    return { id: `cus_${token(10)}`, name: `${given} ${family}`, email: `${given}.${family}${i}@Example.com`, phone: `+${1 + Math.floor(random() * 9)}${String(Math.floor(random() * 1e9)).padStart(9, "0")}`, country, currency: region.currency, address: { line1: `${1 + Math.floor(random() * 400)} ${pick(familyNames)} Street`, city, postalCode, country } };
  });
  const products = Array.from({ length: 40 }, (_, i) => ({ sku: `${["OUT", "CMP", "APP", "ACC"][i % 4]}-${String(1000 + i * 37).padStart(4, "0")}`, name: `${productNames[i % productNames.length]} ${["S", "M", "L", "XL"][i % 4]}`, usdCents: 500 + Math.floor(random() * 25_000) }));
  const priceIn = (usdCents: number, currency: Currency) => (exponents[currency] ? Math.round(usdCents * rates[currency]) : Math.round((usdCents * rates[currency]) / 100));
  const orders: { orderId: string; customer: Customer; totalMinor: number; lines: { sku: string; quantity: number }[] }[] = [];

  let clock = Date.UTC(2026, 2, 2, 8);
  // The time as the sender's system wrote it, in the customer's local offset.
  const stamp = (customer: Customer) => {
    clock += 5_000 + Math.floor(random() * 60_000);
    const offset = pick(regionsByCountry[customer.country].offsets);
    if (offset === 0) return new Date(clock).toISOString();
    const local = new Date(clock + offset * 60_000).toISOString().slice(0, 19);
    const hours = Math.floor(Math.abs(offset) / 60);
    return `${local}${offset < 0 ? "-" : "+"}${String(hours).padStart(2, "0")}:${String(Math.abs(offset) % 60).padStart(2, "0")}`;
  };
  const reference = (customer: Customer) => ({ id: customer.id, email: customer.email });

  const orderEvent = (customer = pick(customers)) => {
    const currency = customer.currency;
    const lines = Array.from({ length: 1 + Math.floor(random() * 4) }, () => {
      const product = pick(products);
      return { sku: product.sku, name: product.name, quantity: 1 + Math.floor(random() * 3), unitPrice: money(priceIn(product.usdCents, currency), currency) };
    });
    const subtotal = lines.reduce((sum, line) => sum + toMinor(line.unitPrice) * line.quantity, 0);
    const method = pick(["standard", "standard", "express", "pickup"] as const);
    const shipping = method === "pickup" ? 0 : priceIn(method === "express" ? 1500 : 500, currency);
    const discounts = random() < 0.2 ? [{ code: pick(["WELCOME10", "SPRING25", "LOYAL5"]), amount: money(Math.floor(subtotal / 10), currency) }] : [];
    const total = subtotal + shipping - discounts.reduce((sum, discount) => sum + toMinor(discount.amount), 0);
    const orderId = `ord_${token(10)}`;
    orders.push({ orderId, customer, totalMinor: total, lines: lines.map(line => ({ sku: line.sku, quantity: line.quantity })) });
    return {
      id: `evt_${token(16)}`,
      type: "order.created",
      occurredAt: stamp(customer),
      data: { orderId, customer: reference(customer), lines, shipping: { method, address: method === "pickup" ? null : customer.address, price: money(shipping, currency) }, discounts, total: money(total, currency) },
    };
  };
  const refundEvent = () => {
    const order = pick(orders);
    const full = random() < 0.5;
    return {
      id: `evt_${token(16)}`,
      type: "order.refunded",
      occurredAt: stamp(order.customer),
      data: { refundId: `re_${token(12)}`, orderId: order.orderId, customer: reference(order.customer), amount: money(full ? order.totalMinor : Math.floor(order.totalMinor / 3), order.customer.currency), reason: pick(["requested_by_customer", "duplicate", "fraudulent", "damaged"] as const), ...(full ? {} : { lines: order.lines.slice(0, 1) }) },
    };
  };
  const customerEvent = (customer = pick(customers)) => {
    const changed = (["email", "name", "phone", "address", "marketingOptIn", "tags"] as const).filter(() => random() < 0.3);
    if (!changed.length) changed.push("marketingOptIn");
    if (changed.includes("email")) customer.email = customer.email.toUpperCase();
    return {
      id: `evt_${token(16)}`,
      type: "customer.updated",
      occurredAt: stamp(customer),
      data: { customer: { id: customer.id, email: customer.email, name: customer.name, phone: random() < 0.8 ? customer.phone : null, address: random() < 0.9 ? customer.address : null, marketingOptIn: random() < 0.5, tags: ["vip", "wholesale", "newsletter", "returning"].filter(() => random() < 0.25) }, changed },
    };
  };

  // Records a sender gets wrong, one fault each; the function must report every one of them.
  type Event = Record<string, any>;
  const faults: (() => Event)[] = [
    () => withData(orderEvent(), data => (data.coupon = "SPRING25")),
    () => withData(orderEvent(), data => (data.lines[0].quantity = 0)),
    () => withData(orderEvent(), data => (data.total.currency = "XYZ")),
    () => withData(orderEvent(), data => (data.total.amount = formatMinor(toMinor(data.total) + 1, data.total.currency))),
    () => withData(customerEvent(), data => (data.customer.email = "ana.silva@")),
    () => withData(refundEvent(), data => (data.amount.amount = `-${data.amount.amount}`)),
    () => withData(orderEvent(), data => (data.lines[0].unitPrice = { amount: "1200.5", currency: "JPY" })),
    () => {
      const event: Event = orderEvent();
      delete event.occurredAt;
      return event;
    },
    () => ({ ...customerEvent(), occurredAt: "2026-02-30T25:00:00Z" }),
    () => ({ ...orderEvent(), type: "order.updated" }),
  ];
  function withData(event: Event, change: (data: Event) => void): Event {
    change(event.data);
    return event;
  }

  let fault = 0;
  return batchSizes.map(size => {
    const events: Event[] = [];
    const build = () => {
      const roll = random();
      return roll < 0.6 || orders.length === 0 ? orderEvent() : roll < 0.75 ? refundEvent() : customerEvent();
    };
    for (let i = 0; i < size - faultsPerBatch - redeliveriesPerBatch; ++i) events.push(build());
    const broken = new Set<Event>();
    for (let i = 0; i < faultsPerBatch; ++i) {
      const event = faults[fault++ % faults.length]();
      broken.add(event);
      events.splice(Math.floor(random() * (events.length + 1)), 0, event);
    }
    // A redelivery repeats an earlier event exactly, later in the batch.
    for (let i = 0; i < redeliveriesPerBatch; ++i) {
      let original: number;
      do original = Math.floor(random() * events.length);
      while (broken.has(events[original]));
      events.splice(original + 1 + Math.floor(random() * (events.length - original)), 0, events[original]);
    }
    const body = JSON.stringify({ batchId: `batch_${token(12)}`, source: pick(["storefront", "pos", "marketplace"]), sentAt: new Date(clock + 30_000).toISOString(), events });
    return { body, events: events.length, invalid: events.flatMap((event, index) => (broken.has(event) ? [index] : [])), redeliveries: redeliveriesPerBatch };
  });
}

// ---------------------------------------------------------------------------------------------------------------------

interface Call {
  path: string;
  contentType: string;
  body: Uint8Array | string;
  check(response: any): string | null; // a problem with the response, or null
}

// Everything a response produced except its Date header, which follows the clock.
function responseText(response: Response, body: string): string {
  let text = `${response.status}\n`;
  for (const [name, value] of response.headers) if (name !== "date") text += `${name}: ${value}\n`;
  return `${text}\n${body}`;
}

export async function setup(): Promise<Workload> {
  // unpdf loads pdf.js on first use; a deployment that extracts on every request loads it while it starts.
  await getResolvedPDFJS();
  const directory = join(import.meta.dir, "../inputs/pdf");
  const documents = readdirSync(directory)
    .filter(name => name.endsWith(".pdf"))
    .sort()
    .map(name => ({ name, bytes: new Uint8Array(readFileSync(join(directory, name))) }));
  if (!documents.length) throw new Error(`${directory} holds no PDF`);
  const batches = buildBatches(generator(0x1a9e57));

  const documentCalls: Call[] = documents.map(({ name, bytes }) => ({
    path: `/documents?name=${encodeURIComponent(name)}`,
    contentType: "application/pdf",
    body: bytes,
    check: response => (response.document?.pages >= 1 && response.chunks?.length >= response.document.pages ? null : `${name} gave ${response.chunks?.length} chunks for ${response.document?.pages} pages`),
  }));
  const batchCalls: Call[] = batches.map((batch, i) => ({
    path: "/events",
    contentType: "application/json",
    body: batch.body,
    check: response => {
      const counts = response.counts;
      const indexes = response.rejected?.map((record: { index: number }) => record.index).join(",");
      if (indexes !== batch.invalid.join(",")) return `batch ${i + 1} rejected the records at ${indexes} where those at ${batch.invalid.join(",")} were due`;
      if (counts.duplicates !== batch.redeliveries || counts.accepted + counts.duplicates + counts.rejected !== batch.events) return `batch ${i + 1} counted ${JSON.stringify(counts)} for ${batch.events} events with ${batch.redeliveries} redeliveries`;
      return null;
    },
  }));
  // The batches arrive between the documents, as webhooks do while uploads run.
  const calls = documentCalls.flatMap((call, i) => (i % 2 === 0 && batchCalls[i / 2] ? [call, batchCalls[i / 2]] : [call]));
  if (calls.length !== documentCalls.length + batchCalls.length) throw new Error("a batch has no place between the documents");

  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    routes: { "/documents": { POST: ingestDocument }, "/events": { POST: ingestEvents } },
    fetch: () => json(404, problem("not_found", "no such route")),
    error(error) {
      console.error(error);
      return json(500, problem("internal", "internal error"));
    },
  });
  const origin = `http://127.0.0.1:${server.port}`;

  return {
    async pass() {
      const output: string[] = [];
      for (const call of calls) {
        const response = await fetch(origin + call.path, { method: "POST", headers: { "content-type": call.contentType }, body: call.body });
        const body = await response.text();
        if (response.status !== 200) throw new Error(`POST ${call.path} answered ${response.status}: ${body.slice(0, 400)}`);
        const problem = call.check(JSON.parse(body));
        if (problem) throw new Error(problem);
        output.push(responseText(response, body));
      }
      return output;
    },
    async close() {
      await server.stop(true);
    },
  };
}
