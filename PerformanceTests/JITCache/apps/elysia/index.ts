import { Elysia, t } from "elysia";
import { jwt } from "@elysiajs/jwt";

type Todo = { id: string; title: string; done: boolean };

const todos = new Map<string, Todo>();
let lastId = 0;

const app = new Elysia()
  .use(jwt({ name: "jwt", secret: "Fischl von Luftschloss Narfidort", iat: false }))
  .post(
    "/login",
    async ({ jwt, body, status }) => {
      if (body.username !== "demo" || body.password !== "demo") return status(401, "Invalid username or password");
      return { token: await jwt.sign({ sub: body.username }) };
    },
    { body: t.Object({ username: t.String(), password: t.String() }) },
  )
  .onBeforeHandle(async ({ jwt, headers, status }) => {
    if (!(await jwt.verify(headers.authorization?.replace("Bearer ", "")))) return status(401, "Unauthorized");
  })
  .get("/todos", () => [...todos.values()])
  .post(
    "/todos",
    ({ body }) => {
      const todo: Todo = { id: String(++lastId), done: false, ...body };
      todos.set(todo.id, todo);
      return todo;
    },
    { body: t.Object({ title: t.String({ minLength: 1, maxLength: 200 }) }) },
  )
  .delete("/todos/:id", ({ params }) => {
    todos.delete(params.id);
    return { ok: true };
  });

export default app;
