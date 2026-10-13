import { z } from "zod";

const currency = z.string().length(3);

const BaseEvent = z.object({
  id: z.string().startsWith("evt_"),
  created: z.number().int().positive(),
});

const WebhookEvent = z.discriminatedUnion("type", [
  BaseEvent.extend({
    type: z.literal("checkout.session.completed"),
    data: z.object({
      id: z.string(),
      customer_email: z.email(),
      amount_total: z.number().int().nonnegative(),
      currency,
    }),
  }),
  BaseEvent.extend({
    type: z.literal("payment_intent.succeeded"),
    data: z.object({ id: z.string(), amount: z.number().int().positive(), currency }),
  }),
  BaseEvent.extend({
    type: z.literal("payment_intent.payment_failed"),
    data: z.object({ id: z.string(), amount: z.number().int().positive(), currency, failure_message: z.string() }),
  }),
  BaseEvent.extend({
    type: z.literal("invoice.paid"),
    data: z.object({ id: z.string(), customer: z.string(), amount_paid: z.number().int().nonnegative(), currency }),
  }),
  BaseEvent.extend({
    type: z.literal("customer.subscription.deleted"),
    data: z.object({ id: z.string(), customer: z.string() }),
  }),
]);

export default {
  async fetch(request: Request) {
    if (request.method !== "POST") return new Response("Method Not Allowed", { status: 405 });

    const result = WebhookEvent.safeParse(await request.json().catch(() => null));
    if (!result.success) return Response.json({ error: "Invalid event", issues: result.error.issues }, { status: 400 });

    return Response.json({ received: true });
  },
};
