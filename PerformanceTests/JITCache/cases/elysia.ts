// The API of a SaaS control plane, the backend a hosting or database platform's dashboard talks to: an Elysia app with
// Elysia's own validation (t schemas on params, query and bodies), CORS for the dashboard's origin, an error handler,
// derived request context and a session check that verifies a JWT bearer token with jose. It serves an in-memory dataset
// of teams, members, projects and deployments through Elysia's listen, which opens Bun.serve. One pass sends it 100
// requests with fetch from the same process, one at a time, in a fixed order: the dashboard sessions of four users who
// read their profile, list, open, create, deploy and delete projects and list team members, interleaved with CORS
// preflights, health checks and requests that carry no token or a forged one.

import { cors } from "@elysiajs/cors";
import { Elysia, t } from "elysia";
import { SignJWT, jwtVerify, type JWTPayload } from "jose";
import type { Workload } from "../run.ts";

// mulberry32: every process draws the same dataset, the same new ids and the same requests.
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

const dashboardOrigin = "https://dashboard.example.com";
const issuer = "https://auth.example.com/";
const audience = "control-plane";
const keyId = "test-2026-01";
// A fixed test key and fixed claims: every process signs the same tokens, so the checksums agree.
const signingKey = new TextEncoder().encode("jitcache-macro-bench/control-plane/hs256-test-key/2026");
const issuedAt = Date.UTC(2026, 0, 1) / 1000;
const expiresAt = Date.UTC(2099, 0, 1) / 1000;
const baseTime = Date.UTC(2026, 0, 1);
const day = 24 * 60 * 60 * 1000;

const frameworks = ["nextjs", "remix", "astro", "sveltekit", "nuxt", "hono", "elysia", "vite", "static"] as const;
const regions = ["iad1", "sfo1", "cdg1", "fra1", "hnd1", "gru1", "syd1"] as const;
const roles = ["owner", "admin", "developer", "viewer"] as const;
const deploymentStates = ["READY", "ERROR", "BUILDING", "QUEUED", "CANCELED"] as const;
const targets = ["production", "preview"] as const;
const envTargets = ["production", "preview", "development"] as const;
const gitProviders = ["github", "gitlab", "bitbucket"] as const;
const plans = ["hobby", "pro", "enterprise"] as const;
const fullScopes = ["projects:read", "projects:write", "deployments:read", "deployments:write", "teams:read"];
const readScopes = ["projects:read", "deployments:read", "teams:read"];

const teamNames = ["Acme", "Globex", "Initech", "Umbrella", "Hooli", "Soylent", "Vandelay", "Tyrell"];
const firstNames = ["Ada", "Alan", "Barbara", "Claude", "Donald", "Edsger", "Frances", "Grace", "Hedy", "Ivan", "John", "Ken", "Leslie", "Margaret", "Niklaus", "Radia"];
const lastNames = ["Lovelace", "Turing", "Liskov", "Shannon", "Knuth", "Dijkstra", "Allen", "Hopper", "Lamarr", "Sutherland", "McCarthy", "Thompson", "Lamport", "Hamilton", "Wirth", "Perlman"];
const projectWords = ["atlas", "beacon", "cobalt", "delta", "ember", "falcon", "harbor", "juniper", "lumen", "nimbus", "orbit", "pioneer", "quartz", "sierra", "tundra", "vertex"];
const projectKinds = ["web", "api", "docs", "dashboard", "worker", "admin", "storefront", "blog", "auth", "billing", "search", "cron"];
const branches = ["main", "main", "main", "develop", "feat/checkout", "feat/search", "fix/login-redirect", "chore/deps", "release/2026.1"];
const commitMessages = ["Fix flaky checkout test", "Bump dependencies", "Add search filters", "Refactor auth middleware", "Update pricing page copy", "Cache product queries", "Handle empty cart", "Add OpenGraph images", "Migrate to the new router", "Tune image sizes", "Log slow queries", "Remove the legacy API client"];
const projectsPerTeam = 60;
const userCount = 48;

type Role = (typeof roles)[number];

interface User {
  id: string;
  name: string;
  email: string;
  avatarUrl: string;
  createdAt: string;
}

interface Member {
  userId: string;
  role: Role;
  joinedAt: string;
}

interface Team {
  id: string;
  slug: string;
  name: string;
  plan: (typeof plans)[number];
  createdAt: string;
  members: Member[];
}

interface Project {
  id: string;
  teamId: string;
  name: string;
  framework: (typeof frameworks)[number];
  region: (typeof regions)[number];
  productionBranch: string;
  git: { provider: (typeof gitProviders)[number]; repo: string } | null;
  // Values are encrypted at rest and never returned, so the store keeps only what the API shows.
  env: { key: string; target: string[]; type: "encrypted" }[];
  createdAt: number;
  updatedAt: number;
  createdBy: string;
}

interface Deployment {
  id: string;
  projectId: string;
  state: (typeof deploymentStates)[number];
  target: (typeof targets)[number];
  branch: string;
  sha: string;
  message: string;
  creatorId: string;
  createdAt: number;
  readyAt: number | null;
  url: string;
}

interface Session {
  userId: string;
  sessionId: string;
  scopes: Set<string>;
}

const iso = (time: number) => new Date(time).toISOString();

class Store {
  readonly users = new Map<string, User>();
  readonly teams = new Map<string, Team>();
  readonly projects = new Map<string, Project>();
  readonly projectsByTeam = new Map<string, Project[]>();
  readonly deployments = new Map<string, Deployment[]>(); // by project, newest first
  readonly memberships = new Map<string, { team: Team; role: Role }[]>(); // by user
  // Records the API creates take their ids from a seeded generator and their times from a clock that advances with
  // every write, so every process creates the same records.
  #random: () => number;
  #clock = baseTime;

  constructor(random: () => number) {
    this.#random = random;
  }

  id(prefix: string, length: number): string {
    let id = prefix;
    for (let i = 0; i < length; ++i) id += "0123456789abcdefghijklmnopqrstuvwxyz"[Math.floor(this.#random() * 36)];
    return id;
  }

  now(): number {
    return (this.#clock += 1000);
  }

  roleOf(teamId: string, userId: string): Role | undefined {
    return this.memberships.get(userId)?.find(membership => membership.team.id === teamId)?.role;
  }

  addProject(project: Project) {
    this.projects.set(project.id, project);
    this.projectsByTeam.get(project.teamId)!.push(project);
    this.deployments.set(project.id, []);
  }

  deleteProject(project: Project) {
    this.projects.delete(project.id);
    const siblings = this.projectsByTeam.get(project.teamId)!;
    siblings.splice(siblings.indexOf(project), 1);
    this.deployments.delete(project.id);
  }
}

// The four users whose dashboards send the authenticated requests, with the teams and roles the requests rely on: an
// owner, a developer, a viewer whose token carries read scopes only, and an admin.
const testUsers: { scopes: string[]; teams: [number, Role][] }[] = [
  { scopes: fullScopes, teams: [[0, "owner"], [2, "admin"], [5, "developer"]] },
  { scopes: fullScopes, teams: [[0, "developer"], [1, "developer"]] },
  { scopes: readScopes, teams: [[1, "viewer"], [0, "viewer"]] },
  { scopes: fullScopes, teams: [[3, "admin"], [4, "developer"]] },
];

function buildStore(random: () => number): Store {
  const store = new Store(random);
  const pick = <T>(list: readonly T[]) => list[Math.floor(random() * list.length)];
  const users: User[] = [];
  for (let i = 0; i < userCount; ++i) {
    const first = firstNames[i % firstNames.length];
    // Each round through the first names shifts the last names by one, so no full name repeats.
    const last = lastNames[(i + Math.floor(i / firstNames.length)) % lastNames.length];
    const id = store.id("usr_", 12);
    const user = { id, name: `${first} ${last}`, email: `${first}.${last}${i}@example.com`.toLowerCase(), avatarUrl: `https://avatars.example.com/${id}.png`, createdAt: iso(baseTime - Math.floor(random() * 1500) * day) };
    users.push(user);
    store.users.set(id, user);
    store.memberships.set(id, []);
  }

  const teams: Team[] = teamNames.map(name => ({ id: store.id("team_", 8), slug: name.toLowerCase(), name, plan: pick(plans), createdAt: iso(baseTime - Math.floor(random() * 1200) * day), members: [] }));
  const join = (team: Team, user: User, role: Role) => {
    team.members.push({ userId: user.id, role, joinedAt: iso(Date.parse(team.createdAt) + Math.floor(random() * 300) * day) });
    store.memberships.get(user.id)!.push({ team, role });
  };
  testUsers.forEach((test, i) => {
    for (const [team, role] of test.teams) join(teams[team], users[i], role);
  });
  for (const team of teams) {
    if (!team.members.some(member => member.role === "owner")) join(team, users[testUsers.length + teams.indexOf(team)], "owner");
    const size = 8 + Math.floor(random() * 10);
    while (team.members.length < size) {
      const user = users[testUsers.length + Math.floor(random() * (userCount - testUsers.length))];
      if (!team.members.some(member => member.userId === user.id)) join(team, user, pick(["admin", "developer", "developer", "developer", "viewer"] as const));
    }
    store.teams.set(team.id, team);
    store.projectsByTeam.set(team.id, []);
  }

  for (const team of teams) {
    const names = new Set<string>();
    while (names.size < projectsPerTeam) names.add(`${pick(projectWords)}-${pick(projectKinds)}`);
    for (const name of names) {
      const createdAt = baseTime - Math.floor(random() * 700) * day;
      const project: Project = {
        id: store.id("prj_", 12),
        teamId: team.id,
        name,
        framework: pick(frameworks),
        region: pick(regions),
        productionBranch: random() < 0.9 ? "main" : "trunk",
        git: random() < 0.9 ? { provider: pick(gitProviders), repo: `${team.slug}/${name}` } : null,
        env: ["DATABASE_URL", "REDIS_URL", "STRIPE_SECRET_KEY", "SENTRY_DSN", "NEXT_PUBLIC_API_URL"].filter(() => random() < 0.6).map(key => ({ key, target: random() < 0.5 ? ["production"] : ["production", "preview"], type: "encrypted" as const })),
        createdAt,
        updatedAt: createdAt + Math.floor(random() * (baseTime - createdAt)),
        createdBy: pick(team.members).userId,
      };
      store.addProject(project);
      const deployments = store.deployments.get(project.id)!;
      let time = project.updatedAt;
      for (let i = Math.floor(random() * 31); i > 0; --i) {
        const roll = random();
        const state = deployments.length === 0 && roll < 0.1 ? pick(["BUILDING", "QUEUED"] as const) : roll < 0.8 ? "READY" : roll < 0.93 ? "ERROR" : "CANCELED";
        const branch = pick(branches);
        const id = store.id("dpl_", 12);
        let sha = "";
        for (let j = 0; j < 40; ++j) sha += "0123456789abcdef"[Math.floor(random() * 16)];
        deployments.push({
          id,
          projectId: project.id,
          state,
          target: branch === project.productionBranch && random() < 0.7 ? "production" : "preview",
          branch,
          sha,
          message: pick(commitMessages),
          creatorId: pick(team.members).userId,
          createdAt: time,
          readyAt: state === "READY" || state === "ERROR" ? time + 20_000 + Math.floor(random() * 300_000) : null,
          url: `${name}-${id.slice(4, 13)}-${team.slug}.example.app`,
        });
        time -= Math.floor(random() * 3 * day);
      }
    }
  }
  return store;
}

const problem = (code: string, message: string, issues?: { path: string; message: string }[]) => ({ error: issues ? { code, message, issues } : { code, message } });

function deploymentView(deployment: Deployment) {
  return {
    id: deployment.id,
    state: deployment.state,
    target: deployment.target,
    url: `https://${deployment.url}`,
    meta: { branch: deployment.branch, commitSha: deployment.sha, commitMessage: deployment.message },
    creator: deployment.creatorId,
    createdAt: iso(deployment.createdAt),
    readyAt: deployment.readyAt === null ? null : iso(deployment.readyAt),
    buildDurationMs: deployment.readyAt === null ? null : deployment.readyAt - deployment.createdAt,
  };
}

function projectSummary(store: Store, project: Project) {
  const latest = store.deployments.get(project.id)![0];
  return {
    id: project.id,
    name: project.name,
    framework: project.framework,
    region: project.region,
    updatedAt: iso(project.updatedAt),
    latestDeployment: latest ? { id: latest.id, state: latest.state, url: `https://${latest.url}`, createdAt: iso(latest.createdAt) } : null,
  };
}

function projectView(store: Store, project: Project) {
  const deployments = store.deployments.get(project.id)!;
  const production = deployments.find(deployment => deployment.target === "production" && deployment.state === "READY");
  return {
    id: project.id,
    teamId: project.teamId,
    name: project.name,
    framework: project.framework,
    region: project.region,
    link: project.git ? { ...project.git, productionBranch: project.productionBranch } : null,
    env: project.env,
    createdAt: iso(project.createdAt),
    updatedAt: iso(project.updatedAt),
    createdBy: project.createdBy,
    deploymentCount: deployments.length,
    production: production ? deploymentView(production) : null,
    latestDeployments: deployments.slice(0, 3).map(deploymentView),
  };
}

const teamId = t.String({ pattern: "^team_[a-z0-9]{8}$" });
const projectParams = t.Object({ id: t.String({ pattern: "^prj_[a-z0-9]{12}$" }) });

// The session check. derive gives every request its id and bearer token, and the auth macro's resolve verifies the token
// on the routes that declare auth: true, giving their handlers the user and the session, or answers 401.
function authentication(store: Store) {
  return new Elysia({ name: "authentication" })
    .derive({ as: "global" }, ({ headers, set }) => {
      const requestId = headers["x-request-id"] ?? "none";
      // Every response names its request, for support tickets and log searches.
      set.headers["x-request-id"] = requestId;
      const authorization = headers.authorization;
      return { requestId, bearer: authorization?.startsWith("Bearer ") ? authorization.slice("Bearer ".length) : null };
    })
    .macro({
      auth: {
        async resolve({ bearer, set, status }) {
          if (!bearer) {
            set.headers["www-authenticate"] = 'Bearer realm="control-plane"';
            return status(401, problem("unauthorized", "missing bearer token"));
          }
          const reject = (message: string) => {
            set.headers["www-authenticate"] = 'Bearer realm="control-plane", error="invalid_token"';
            return status(401, problem("unauthorized", message));
          };
          let payload: JWTPayload;
          try {
            ({ payload } = await jwtVerify(bearer, signingKey, { issuer, audience, algorithms: ["HS256"] }));
          } catch (error) {
            return reject((error as { code?: string }).code === "ERR_JWT_EXPIRED" ? "token expired" : "invalid token");
          }
          const user = typeof payload.sub === "string" ? store.users.get(payload.sub) : undefined;
          if (!user) return reject("unknown subject");
          const session: Session = { userId: user.id, sessionId: String(payload.sid), scopes: new Set(String(payload.scope ?? "").split(" ")) };
          return { user, session };
        },
      },
    });
}

function buildApp(store: Store) {
  const forbidden = (message: string) => problem("forbidden", message);
  const missingScope = (scope: string) => problem("insufficient_scope", `the token lacks ${scope}`);
  return new Elysia({ name: "control-plane" })
    .use(
      cors({
        origin: [dashboardOrigin],
        methods: ["GET", "POST", "PATCH", "DELETE", "OPTIONS"],
        allowedHeaders: ["authorization", "content-type", "x-request-id"],
        exposeHeaders: ["x-request-id", "x-total-count", "location"],
        credentials: true,
        maxAge: 600,
      }),
    )
    .onError(({ code, error, set }) => {
      switch (code) {
        case "VALIDATION":
          // Elysia answers 422; the body lists every failed check, so the dashboard can mark each field.
          set.status = 422;
          return problem(
            "invalid_request",
            `invalid ${error.type}`,
            error.all.map(issue => ({ path: "path" in issue ? issue.path : "", message: "summary" in issue && issue.summary ? issue.summary : issue.message })),
          );
        case "NOT_FOUND":
          set.status = 404;
          return problem("not_found", "no such route");
        case "PARSE":
          set.status = 400;
          return problem("malformed_body", "the body is not valid JSON");
        default:
          // A deployment logs the error for its on-call engineer and tells the client nothing more.
          console.error(error);
          set.status = 500;
          return problem("internal", "internal error");
      }
    })
    .use(authentication(store))
    .get("/health", () => ({ status: "ok", projects: store.projects.size }))
    .group("/v1", app =>
      app
        .get(
          "/me",
          ({ user, session }) => ({
            id: user.id,
            name: user.name,
            email: user.email,
            avatarUrl: user.avatarUrl,
            createdAt: user.createdAt,
            session: { id: session.sessionId, scopes: [...session.scopes] },
            teams: store.memberships.get(user.id)!.map(({ team, role }) => ({ id: team.id, slug: team.slug, name: team.name, plan: team.plan, role })),
          }),
          { auth: true },
        )
        .get(
          "/projects",
          ({ query, user, session, set, status }) => {
            if (!session.scopes.has("projects:read")) return status(403, missingScope("projects:read"));
            if (!store.roleOf(query.teamId, user.id)) return status(403, forbidden("not a member of this team"));
            let projects = store.projectsByTeam.get(query.teamId)!;
            if (query.search) {
              const needle = query.search.toLowerCase();
              projects = projects.filter(project => project.name.includes(needle) || project.git?.repo.includes(needle));
            }
            if (query.framework) projects = projects.filter(project => project.framework === query.framework);
            const sort = query.sort ?? "updatedAt";
            const direction = query.order === "asc" ? 1 : -1;
            projects = projects.toSorted((a, b) => (a[sort] < b[sort] ? -direction : a[sort] > b[sort] ? direction : a.id < b.id ? -1 : 1));
            const page = query.page ?? 1;
            const limit = query.limit ?? 20;
            set.headers["x-total-count"] = String(projects.length);
            return {
              data: projects.slice((page - 1) * limit, page * limit).map(project => projectSummary(store, project)),
              pagination: { page, limit, total: projects.length, pages: Math.max(1, Math.ceil(projects.length / limit)) },
            };
          },
          {
            auth: true,
            query: t.Object({
              teamId,
              search: t.Optional(t.String({ minLength: 1, maxLength: 64 })),
              framework: t.Optional(t.UnionEnum(frameworks)),
              sort: t.Optional(t.UnionEnum(["name", "createdAt", "updatedAt"])),
              order: t.Optional(t.UnionEnum(["asc", "desc"])),
              page: t.Optional(t.Integer({ minimum: 1, maximum: 1000 })),
              limit: t.Optional(t.Integer({ minimum: 1, maximum: 100 })),
            }),
          },
        )
        .post(
          "/projects",
          ({ body, user, session, set, status }) => {
            if (!session.scopes.has("projects:write")) return status(403, missingScope("projects:write"));
            const role = store.roleOf(body.teamId, user.id);
            if (!role) return status(403, forbidden("not a member of this team"));
            if (role === "viewer") return status(403, forbidden("viewers cannot create projects"));
            if (store.projectsByTeam.get(body.teamId)!.some(project => project.name === body.name)) return status(409, problem("conflict", `the team already has a project named ${body.name}`));
            const now = store.now();
            const project: Project = {
              id: store.id("prj_", 12),
              teamId: body.teamId,
              name: body.name,
              framework: body.framework,
              region: body.region ?? "iad1",
              productionBranch: body.gitRepository?.productionBranch ?? "main",
              git: body.gitRepository ? { provider: body.gitRepository.provider, repo: body.gitRepository.repo } : null,
              env: (body.environmentVariables ?? []).map(variable => ({ key: variable.key, target: variable.target, type: "encrypted" as const })),
              createdAt: now,
              updatedAt: now,
              createdBy: user.id,
            };
            store.addProject(project);
            set.headers.location = `/v1/projects/${project.id}`;
            return status(201, projectView(store, project));
          },
          {
            auth: true,
            body: t.Object({
              teamId,
              name: t.String({ pattern: "^[a-z0-9](?:[a-z0-9-]{0,50}[a-z0-9])?$" }),
              framework: t.UnionEnum(frameworks),
              region: t.Optional(t.UnionEnum(regions)),
              gitRepository: t.Optional(
                t.Object({
                  provider: t.UnionEnum(gitProviders),
                  repo: t.String({ pattern: "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$" }),
                  productionBranch: t.Optional(t.String({ minLength: 1, maxLength: 255 })),
                }),
              ),
              environmentVariables: t.Optional(
                t.Array(
                  t.Object({
                    key: t.String({ pattern: "^[A-Z_][A-Z0-9_]{0,63}$" }),
                    value: t.String({ maxLength: 4096 }),
                    target: t.Array(t.UnionEnum(envTargets), { minItems: 1, uniqueItems: true }),
                  }),
                  { maxItems: 50 },
                ),
              ),
            }),
          },
        )
        .get(
          "/projects/:id",
          ({ params, user, status }) => {
            const project = store.projects.get(params.id);
            // A project of another team answers like a missing one, so ids reveal nothing.
            if (!project || !store.roleOf(project.teamId, user.id)) return status(404, problem("not_found", "project not found"));
            return projectView(store, project);
          },
          { auth: true, params: projectParams },
        )
        .delete(
          "/projects/:id",
          ({ params, user, session, status }) => {
            if (!session.scopes.has("projects:write")) return status(403, missingScope("projects:write"));
            const project = store.projects.get(params.id);
            const role = project && store.roleOf(project.teamId, user.id);
            if (!project || !role) return status(404, problem("not_found", "project not found"));
            if (role === "viewer") return status(403, forbidden("viewers cannot delete projects"));
            store.deleteProject(project);
            return new Response(null, { status: 204 });
          },
          { auth: true, params: projectParams },
        )
        .get(
          "/projects/:id/deployments",
          ({ params, query, user, session, status }) => {
            if (!session.scopes.has("deployments:read")) return status(403, missingScope("deployments:read"));
            const project = store.projects.get(params.id);
            if (!project || !store.roleOf(project.teamId, user.id)) return status(404, problem("not_found", "project not found"));
            const limit = query.limit ?? 20;
            const matches: Deployment[] = [];
            let more = false;
            for (const deployment of store.deployments.get(project.id)!) {
              if (query.until !== undefined && deployment.createdAt >= query.until) continue;
              if (query.state && deployment.state !== query.state) continue;
              if (query.target && deployment.target !== query.target) continue;
              if (query.branch && deployment.branch !== query.branch) continue;
              if (matches.length === limit) {
                more = true;
                break;
              }
              matches.push(deployment);
            }
            return { data: matches.map(deploymentView), pagination: { count: matches.length, next: more ? matches.at(-1)!.createdAt : null } };
          },
          {
            auth: true,
            params: projectParams,
            query: t.Object({
              state: t.Optional(t.UnionEnum(deploymentStates)),
              target: t.Optional(t.UnionEnum(targets)),
              branch: t.Optional(t.String({ minLength: 1, maxLength: 255 })),
              until: t.Optional(t.Integer({ minimum: 0 })),
              limit: t.Optional(t.Integer({ minimum: 1, maximum: 100 })),
            }),
          },
        )
        .post(
          "/projects/:id/deployments",
          ({ params, body, user, session, set, status }) => {
            if (!session.scopes.has("deployments:write")) return status(403, missingScope("deployments:write"));
            const project = store.projects.get(params.id);
            const role = project && store.roleOf(project.teamId, user.id);
            if (!project || !role) return status(404, problem("not_found", "project not found"));
            if (role === "viewer") return status(403, forbidden("viewers cannot deploy"));
            const id = store.id("dpl_", 12);
            const team = store.teams.get(project.teamId)!;
            const deployment: Deployment = {
              id,
              projectId: project.id,
              state: "QUEUED",
              target: body.target,
              branch: body.gitSource.ref,
              sha: body.gitSource.sha,
              message: body.meta?.commitMessage ?? "",
              creatorId: user.id,
              createdAt: store.now(),
              readyAt: null,
              url: `${project.name}-${id.slice(4, 13)}-${team.slug}.example.app`,
            };
            store.deployments.get(project.id)!.unshift(deployment);
            project.updatedAt = deployment.createdAt;
            set.headers.location = `/v1/deployments/${id}`;
            return status(201, deploymentView(deployment));
          },
          {
            auth: true,
            params: projectParams,
            body: t.Object({
              target: t.UnionEnum(targets),
              gitSource: t.Object({ ref: t.String({ minLength: 1, maxLength: 255 }), sha: t.String({ pattern: "^[0-9a-f]{40}$" }) }),
              meta: t.Optional(t.Record(t.String(), t.String({ maxLength: 256 }))),
            }),
          },
        )
        .get(
          "/teams/:id/members",
          ({ params, query, user, session, status }) => {
            if (!session.scopes.has("teams:read")) return status(403, missingScope("teams:read"));
            const team = store.teams.get(params.id);
            if (!team || !store.roleOf(team.id, user.id)) return status(403, forbidden("not a member of this team"));
            const needle = query.search?.toLowerCase();
            const members = team.members
              .filter(member => !query.role || member.role === query.role)
              .map(member => ({ member, user: store.users.get(member.userId)! }))
              .filter(({ user }) => !needle || user.name.toLowerCase().includes(needle) || user.email.includes(needle))
              .sort((a, b) => roles.indexOf(a.member.role) - roles.indexOf(b.member.role) || a.user.name.localeCompare(b.user.name, "en"));
            return {
              team: { id: team.id, slug: team.slug, name: team.name },
              members: members.map(({ member, user }) => ({ id: user.id, name: user.name, email: user.email, avatarUrl: user.avatarUrl, role: member.role, joinedAt: member.joinedAt })),
            };
          },
          {
            auth: true,
            params: t.Object({ id: teamId }),
            query: t.Object({ role: t.Optional(t.UnionEnum(roles)), search: t.Optional(t.String({ minLength: 1, maxLength: 64 })) }),
          },
        ),
    );
}

// ---------------------------------------------------------------------------------------------------------------------
// The client

interface Tokens {
  valid: string[]; // one per test user
  tamperedSignature: string;
  tamperedClaims: string;
  expired: string;
  foreignKey: string;
}

async function issueTokens(store: Store): Promise<Tokens> {
  const users = [...store.users.values()];
  const sign = (index: number, expiration: number, key = signingKey, iat = issuedAt) =>
    new SignJWT({ sid: `sess_${String(index + 1).padStart(4, "0")}`, scope: testUsers[index].scopes.join(" "), email: users[index].email, name: users[index].name })
      .setProtectedHeader({ alg: "HS256", typ: "JWT", kid: keyId })
      .setSubject(users[index].id)
      .setIssuer(issuer)
      .setAudience(audience)
      .setIssuedAt(iat)
      .setExpirationTime(expiration)
      .sign(key);
  const valid = await Promise.all(testUsers.map((_, index) => sign(index, expiresAt)));
  const [header, payload, signature] = valid[2].split(".");
  // The viewer's token with one signature character changed, and with its claims rewritten to the owner's with every
  // scope while the viewer's signature stays.
  const changed = signature[10] === "A" ? "B" : "A";
  const claims = { ...JSON.parse(Buffer.from(payload, "base64url").toString()), sub: users[0].id, scope: fullScopes.join(" ") };
  return {
    valid,
    tamperedSignature: `${header}.${payload}.${signature.slice(0, 10)}${changed}${signature.slice(11)}`,
    tamperedClaims: `${header}.${Buffer.from(JSON.stringify(claims)).toString("base64url")}.${signature}`,
    expired: await sign(1, Date.UTC(2025, 6, 1) / 1000, signingKey, Date.UTC(2025, 5, 1) / 1000),
    foreignKey: await sign(3, expiresAt, new TextEncoder().encode("a key that some other issuer holds, 2026")),
  };
}

type Created = Map<string, string>;

interface Call {
  method: "GET" | "POST" | "DELETE" | "OPTIONS";
  path: string | ((created: Created) => string);
  headers: Record<string, string>;
  body?: string;
  status: number; // the status the call must get
  browser: boolean; // sent by the dashboard, so it carries its Origin and must get CORS headers
  capture?: string; // keeps the id of the project the response creates under this name
}

function buildCalls(random: () => number, store: Store, tokens: Tokens): Call[] {
  const pick = <T>(list: readonly T[]) => list[Math.floor(random() * list.length)];
  const users = [...store.users.values()];
  const teams = [...store.teams.values()];
  const teamOf = (user: number, which: number) => teams[testUsers[user].teams[which][0]];
  const projectsOf = (team: Team) => store.projectsByTeam.get(team.id)!;
  const request = (method: Call["method"], path: Call["path"], status: number, options: { token?: string; body?: object; capture?: string; browser?: boolean } = {}): Call => {
    const headers: Record<string, string> = {};
    const browser = options.browser ?? true;
    if (browser) headers.origin = dashboardOrigin;
    if (options.token) headers.authorization = `Bearer ${options.token}`;
    if (options.body) headers["content-type"] = "application/json";
    return { method, path, headers, body: options.body && JSON.stringify(options.body), status, browser, capture: options.capture };
  };
  const as = (user: number) => tokens.valid[user];
  const preflight = (path: string, method: string): Call => {
    const call = request("OPTIONS", path, 204);
    call.headers["access-control-request-method"] = method;
    call.headers["access-control-request-headers"] = "authorization, content-type, x-request-id";
    return call;
  };
  const listQueries = (user: number) => {
    const team = teamOf(user, 0);
    const other = teamOf(user, 1);
    return [
      `teamId=${team.id}`,
      `teamId=${team.id}&search=${pick(projectWords)}`,
      `teamId=${other.id}&framework=${pick(frameworks)}&sort=name&order=asc`,
      `teamId=${team.id}&page=2&limit=25&sort=createdAt`,
    ];
  };

  // Each flow keeps its order; the flows interleave below. A browsing flow is one user's dashboard session.
  const flows: Call[][] = [];
  for (let user = 0; user < testUsers.length; ++user) {
    const flow: Call[] = [preflight("/v1/me", "GET"), request("GET", "/v1/me", 200, { token: as(user) })];
    for (const query of listQueries(user)) flow.push(request("GET", `/v1/projects?${query}`, 200, { token: as(user) }));
    for (let i = 0; i < 2; ++i) flow.push(request("GET", `/v1/projects/${pick(projectsOf(teamOf(user, i))).id}`, 200, { token: as(user) }));
    // The viewer opens fewer deployment lists, which leaves room for the invalid one below.
    const deploymentFilters = ["state=READY&limit=5", "target=production", `branch=main&until=${baseTime - 30 * day}`].slice(0, user === 2 ? 2 : 3);
    for (const filter of deploymentFilters) flow.push(request("GET", `/v1/projects/${pick(projectsOf(teamOf(user, 0))).id}/deployments?${filter}`, 200, { token: as(user) }));
    flow.push(request("GET", `/v1/teams/${teamOf(user, 0).id}/members`, 200, { token: as(user) }));
    flow.push(request("GET", "/v1/me", 200, { token: as(user) }));
    flows.push(flow);
  }

  const outsider = (user: number) => teams.find(team => !store.roleOf(team.id, users[user].id))!;
  const someProject = (user: number) => projectsOf(teamOf(user, 0))[3].id;
  // Requests that fail: validation (422), membership (403), ids no team owns (404).
  flows.push(
    [request("GET", `/v1/projects?teamId=${teamOf(0, 0).id}&limit=500`, 422, { token: as(0) }), request("GET", `/v1/projects/prj_${"0".repeat(12)}`, 404, { token: as(0) })],
    [request("GET", `/v1/projects?teamId=${teamOf(1, 0).id}&sort=popularity`, 422, { token: as(1) }), request("GET", `/v1/projects?teamId=${outsider(1).id}`, 403, { token: as(1) })],
    [request("GET", "/v1/projects/prj_!!", 422, { token: as(2) }), request("GET", "/v1/teams/team_!!/members", 422, { token: as(2) })],
    [request("GET", `/v1/projects?teamId=${outsider(3).id}`, 403, { token: as(3) }), request("GET", `/v1/projects/${someProject(3)}/deployments?state=done`, 422, { token: as(3) })],
    [request("GET", `/v1/teams/${teamOf(0, 1).id}/members?role=developer`, 200, { token: as(0) }), request("GET", `/v1/teams/${outsider(1).id}/members`, 403, { token: as(1) })],
    [request("GET", `/v1/teams/${teamOf(2, 0).id}/members?search=${firstNames[3].toLowerCase()}`, 200, { token: as(2) })],
  );

  // Project lifecycles: create, deploy and delete, as a preview environment does, so the dataset is the same at the end
  // of every pass. The first one also tries to create its project twice.
  const sha = () => Array.from({ length: 40 }, () => "0123456789abcdef"[Math.floor(random() * 16)]).join("");
  const lifecycles: [number, number, string, object][] = [
    [0, 0, "launch-week-site", { framework: "nextjs", region: "iad1", gitRepository: { provider: "github", repo: "acme/launch-week-site" } }],
    [0, 1, "billing-portal", { framework: "remix", region: "fra1", gitRepository: { provider: "gitlab", repo: "initech/billing-portal", productionBranch: "release" }, environmentVariables: [{ key: "DATABASE_URL", value: "postgres://billing@db.internal:5432/billing", target: ["production", "preview"] }, { key: "STRIPE_SECRET_KEY", value: "sk_test_0000000000000000", target: ["production"] }] }],
    [1, 0, "pr-2041-preview", { framework: "astro" }],
    [1, 1, "status-page", { framework: "static", region: "cdg1" }],
    [3, 0, "edge-api", { framework: "hono", region: "sfo1", gitRepository: { provider: "github", repo: "umbrella/edge-api" } }],
    [3, 1, "docs-v2", { framework: "sveltekit", environmentVariables: [{ key: "SEARCH_API_KEY", value: "search-test-key", target: ["production", "preview", "development"] }] }],
  ];
  lifecycles.forEach(([user, which, name, rest], i) => {
    const key = `project-${i}`;
    const body = { teamId: teamOf(user, which).id, name, ...rest };
    const projectPath = (created: Created) => `/v1/projects/${created.get(key)}`;
    const flow: Call[] = [];
    if (i % 3 === 0) flow.push(preflight("/v1/projects", "POST"));
    flow.push(request("POST", "/v1/projects", 201, { token: as(user), body, capture: key }));
    if (i === 0) flow.push(request("POST", "/v1/projects", 409, { token: as(user), body }));
    flow.push(request("POST", created => `${projectPath(created)}/deployments`, 201, { token: as(user), body: { target: i % 2 ? "preview" : "production", gitSource: { ref: i % 2 ? `feat/${name}` : "main", sha: sha() }, meta: { commitMessage: pick(commitMessages) } } }));
    flow.push(request("DELETE", projectPath, 204, { token: as(user) }));
    flows.push(flow);
  });
  flows.push(
    [request("POST", "/v1/projects", 422, { token: as(1), body: { teamId: teamOf(1, 0).id, name: "My Project", framework: "nextjs" } })],
    [request("POST", "/v1/projects", 422, { token: as(0), body: { teamId: teamOf(0, 0).id, name: "env-check", framework: "vite", environmentVariables: [{ key: "api_url", value: "https://api.example.com", target: [] }] } })],
    [request("POST", "/v1/projects", 403, { token: as(2), body: { teamId: teamOf(2, 0).id, name: "viewer-attempt", framework: "nuxt" } })],
  );

  // Requests a session check must turn away.
  const project = someProject(0);
  flows.push(
    [request("GET", "/v1/me", 401), request("GET", `/v1/projects?teamId=${teamOf(0, 0).id}`, 401)],
    [request("GET", "/v1/me", 401), request("GET", `/v1/projects?teamId=${teamOf(1, 0).id}`, 401)],
    [request("GET", "/v1/me", 401, { token: tokens.tamperedSignature }), request("GET", `/v1/projects/${project}`, 401, { token: tokens.tamperedSignature })],
    [request("GET", "/v1/me", 401, { token: tokens.tamperedClaims }), request("POST", "/v1/projects", 401, { token: tokens.tamperedClaims, body: { teamId: teamOf(0, 0).id, name: "escalated", framework: "vite" } })],
    [request("GET", "/v1/me", 401, { token: tokens.expired }), request("GET", `/v1/teams/${teamOf(1, 0).id}/members`, 401, { token: tokens.expired })],
    [request("GET", "/v1/me", 401, { token: tokens.foreignKey }), request("GET", `/v1/projects/${project}/deployments`, 401, { token: tokens.foreignKey })],
  );
  // The load balancer's health checks, which come from no browser.
  flows.push([request("GET", "/health", 200, { browser: false })], [request("GET", "/health", 200, { browser: false })]);

  // A seeded merge of the flows, each step taking the next call of a flow drawn in proportion to the calls it has left,
  // fixes one interleaving for every pass and every process.
  const calls: Call[] = [];
  const positions = flows.map(() => 0);
  let remaining = flows.reduce((sum, flow) => sum + flow.length, 0);
  while (remaining > 0) {
    let draw = Math.floor(random() * remaining);
    let index = 0;
    while (draw >= flows[index].length - positions[index]) {
      draw -= flows[index].length - positions[index];
      ++index;
    }
    calls.push(flows[index][positions[index]++]);
    --remaining;
  }
  return calls;
}

// Everything a response produced except its Date header, which follows the clock.
function responseText(response: Response, body: string): string {
  let text = `${response.status}\n`;
  for (const [name, value] of response.headers) if (name !== "date") text += `${name}: ${value}\n`;
  return `${text}\n${body}`;
}

export async function setup(): Promise<Workload> {
  const random = generator(0xe1a5);
  const store = buildStore(random);
  const tokens = await issueTokens(store);
  const calls = buildCalls(random, store, tokens);
  if (calls.length !== 100) throw new Error(`a pass holds ${calls.length} calls where 100 were due`);

  const app = buildApp(store).listen({ hostname: "127.0.0.1", port: 0 });
  const origin = `http://127.0.0.1:${app.server!.port}`;

  return {
    async pass() {
      const created: Created = new Map();
      const output: string[] = [];
      for (const [index, call] of calls.entries()) {
        const path = typeof call.path === "string" ? call.path : call.path(created);
        const response = await fetch(origin + path, { method: call.method, headers: { ...call.headers, "x-request-id": `req_${String(index + 1).padStart(3, "0")}` }, body: call.body });
        const body = await response.text();
        const label = `${call.method} ${path}`;
        if (response.status !== call.status) throw new Error(`${label} answered ${response.status} where ${call.status} was due: ${body.slice(0, 400)}`);
        if (call.browser && response.headers.get("access-control-allow-origin") !== dashboardOrigin) throw new Error(`${label} answered without the dashboard's CORS headers`);
        if (call.capture) created.set(call.capture, JSON.parse(body).id);
        output.push(responseText(response, body));
      }
      return output;
    },
    async close() {
      await app.stop(true);
    },
  };
}
