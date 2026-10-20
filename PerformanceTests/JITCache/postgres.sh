#!/usr/bin/env bash
# The PostgreSQL server the postgres case queries: a throwaway container of a fixed PostgreSQL release on the host's
# network, listening on 127.0.0.1:55432, which apps/postgres/index.ts names. start runs it and seeds users_bun_bench as
# Bun's bench/postgres/index.mjs expects, with 100 users, but with birth dates computed from the row number where
# upstream draws them at random, so every run reads the same rows. stop removes the container and its data.
#
#   ./postgres.sh start [cpus]   cpus is a docker --cpuset-cpus list, by default every CPU but the highest, the one
#                                bench.ts pins its Bun processes to unless its --cpu names another; on a host with SMT,
#                                name a list without that CPU's sibling too, which shares its core
#   ./postgres.sh status
#   ./postgres.sh stop
#
# It needs only docker; psql runs inside the container. The container uses the host's network, so no proxy process
# stands between the bench and the server, and its CPUs are restricted, so the server's backends stay off the CPU the
# measured process runs on.
set -euo pipefail

image=postgres:16.14-alpine3.24
name=jitcache-bench-postgres
port=55432

psql_in_container() {
  docker exec --interactive "$name" psql --quiet --no-psqlrc --set=ON_ERROR_STOP=1 --username=postgres "$@"
}

case "${1:-}" in
  start)
    if docker container inspect "$name" >/dev/null 2>&1; then
      echo "postgres.sh: the container $name exists already; ./postgres.sh stop removes it" >&2
      exit 1
    fi
    # On the host's network another server on the port would answer the readiness check and take the bench's queries.
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
      echo "postgres.sh: something already listens on 127.0.0.1:$port" >&2
      exit 1
    fi
    highest=$(($(getconf _NPROCESSORS_ONLN) - 1))
    cpus=${2:-$( ((highest > 0)) && echo "0-$((highest - 1))" || echo 0)}
    # PGPORT is the server's port and the default of every client in the container. The server the image's entrypoint
    # starts while it initializes the data directory listens on no TCP address, so the first TCP connection that
    # succeeds reaches the final server.
    docker run --detach --name "$name" --network host --cpuset-cpus "$cpus" --env POSTGRES_HOST_AUTH_METHOD=trust --env PGPORT="$port" "$image" -c listen_addresses=127.0.0.1 >/dev/null
    for ((attempt = 0; ; ++attempt)); do
      if [[ $(docker container inspect --format '{{.State.Running}}' "$name") != true ]]; then
        docker logs "$name" >&2
        docker rm --force --volumes "$name" >/dev/null
        echo "postgres.sh: the container stopped and was removed; its log is above" >&2
        exit 1
      fi
      docker exec "$name" pg_isready --quiet --host=127.0.0.1 && break
      if ((attempt == 120)); then
        docker rm --force --volumes "$name" >/dev/null
        echo "postgres.sh: the server did not accept connections within 60 seconds; the container was removed" >&2
        exit 1
      fi
      sleep 0.5
    done
    psql_in_container <<'SQL'
CREATE TABLE IF NOT EXISTS "users_bun_bench" (
  id SERIAL PRIMARY KEY,
  first_name TEXT NOT NULL,
  last_name TEXT NOT NULL,
  email TEXT NOT NULL UNIQUE,
  dob TEXT NOT NULL
);
INSERT INTO users_bun_bench (first_name, last_name, email, dob)
SELECT 'FirstName' || i, 'LastName' || i, 'user' || i || '@example.com', to_char(make_date(1970 + i % 30, i % 12 + 1, i % 28 + 1), 'YYYY-MM-DD')
FROM generate_series(0, 99) AS i
ORDER BY i;
SQL
    echo "PostgreSQL $(psql_in_container --tuples-only --no-align --command='SHOW server_version') runs in $name on 127.0.0.1:$port, CPUs $cpus, with $(psql_in_container --tuples-only --no-align --command='SELECT count(*) FROM users_bun_bench') users"
    ;;
  status)
    if [[ $(docker container inspect --format '{{.State.Running}}' "$name" 2>/dev/null) != true ]]; then
      echo "$name is not running"
      exit 1
    fi
    echo "$name runs $(docker container inspect --format '{{.Config.Image}}, CPUs {{.HostConfig.CpusetCpus}}' "$name"), with $(psql_in_container --tuples-only --no-align --command='SELECT count(*) FROM users_bun_bench') users"
    ;;
  stop)
    # The image declares its data directory a volume, which only --volumes removes with the container.
    docker rm --force --volumes "$name" >/dev/null
    echo "removed $name"
    ;;
  *)
    sed -n '2,/^set /s/^# \{0,1\}//p' "$0" >&2
    exit 2
    ;;
esac
