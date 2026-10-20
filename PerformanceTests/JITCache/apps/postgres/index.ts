// Bun's bench/postgres/index.mjs with Bun's SQL client. Its setup runs while the module loads, so startup includes the
// connection and its two queries, and a call runs one of its batches: 100 concurrent selects awaited together. The
// database is the container postgres.sh starts and seeds, named here, since the bench gives its processes no
// DATABASE_URL or PG* variable for Bun.sql to read. Where upstream would insert users with random birth dates, the app
// stops instead, so every run reads the rows postgres.sh wrote.
import { SQL } from "bun";

const sql = new SQL("postgres://postgres@127.0.0.1:55432/postgres");

// Create the table if it doesn't exist
await sql`
    CREATE TABLE IF NOT EXISTS "users_bun_bench" (
      id SERIAL PRIMARY KEY,
      first_name TEXT NOT NULL,
      last_name TEXT NOT NULL,
      email TEXT NOT NULL UNIQUE,
      dob TEXT NOT NULL
    )
  `;

// Check if users already exist
const existingUsers = await sql`SELECT COUNT(*) as count FROM "users_bun_bench"`;

if (+(existingUsers?.[0]?.count ?? existingUsers?.count) < 100) {
  throw new Error("users_bun_bench holds fewer than 100 users: ./postgres.sh start seeds it");
}

export default {
  async fetch() {
    const promises = [];
    for (let i = 0; i < 100; i++) promises.push(sql`SELECT * FROM "users_bun_bench" LIMIT 100`);
    const results = await Promise.all(promises);
    return Response.json({ queries: results.length, rows: results.reduce((total, rows) => total + rows.length, 0), first: results[0][0], last: results.at(-1).at(-1) });
  },
};
