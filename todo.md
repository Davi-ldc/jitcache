# First priority

Make body identity function-relative, so that an app whose source changed still imports every function that did not change. Today a root is identified by a digest of its whole text and each nested function by its parent's key, table and index, so one edit makes every body of a bundle miss; for Bun 1.4.3 a changed app turns JITCache off with a warning instead. A function's UCB does not depend on where the function sits (`CodeBlock::expressionInfoForBytecodeIndex` adds the function's `sourceOffset()` to the positions its UCB stores), and the holder digest of its UFE already covers everything else its generation reads. A key made of the function's own text digest and a holder digest without absolute positions would therefore let unchanged functions keep importing across deploys. Measure first how many functions keep identical text when a bench app is edited a little and rebuilt with `bun build`, with and without `--minify`. Two identical functions would share one body, which baked facts and the richness rule keep sound.

Support multithreading and workers later. A `child_process.fork` child that runs another entry file inherits the parent's `--jitcache` flags but gets another app digest, so for Bun 1.4.3 it is refused (Consumer parent) or turned off by the producer lock (ConsumerProducer parent) and runs without JITCache. Function-relative identity removes the app gate; until then Bun could pass the parent's digest to forked children.

# Cool ideas
* analisar somar profiles ao invés de ficar com o melhor.

## Compact

Investigar a fração de CPU atribuída ao corpo em relação à CPU total da execução como um sinal adicional para a heurística de `compact`, separando trabalho de CPU de espera por I/O. A coleta, sua atribuição e seu overhead precisam ser avaliados antes de entrar no desenho; por enquanto, a poda usa apenas o aquecimento restaurável e os bytes efetivamente recuperáveis.

Analisar a possibilidade de `compact` comprimir strings e outros dados duplicados para reduzir o tamanho do artefato. O desenho atual aceita duplicação entre arquivos de corpos e os mantém autocontidos, sem compressão.

Measured for that question: an elysia producer's pack under 1.4.2 held 700 bodies in 1.89 MB, 54 of them with an image. `ucb.core` took 41%, `image.baseline` 24% (a quarter of it fixups, 24 bytes each), `ucb.identity` and `ucb.feedback` about 10% each, and the fields stored more than once (the body key four times, the commit identifier and size three times, the header digest in every envelope) about 6%, at no measurable time.

## Decode

The core decode is the largest part of an import's request point now that a module's analysis travels with its body. On the A1 under 1.4.2 it took a median 3.7 µs per body, about 7 ns per core byte: 2.5 ms per hono consumer and 4.3 ms per elysia consumer, about 5% of their startup samples. Trimming the decoder saved under 0.4 ms per process (SPEC-ucb history, "Decode shortcuts were measured and dropped"). A core laid out to be adopted in place instead of rebuilt might halve the decode, about 1 to 2 ms in elysia, at the cost of a new codec and its twins. Measure on 1.4.3 where the decode's time goes by body kind and size before designing anything.

## otimizar tempo para publicar
* disco é sync e a thread principal fica esperando a escrita rolar. Pensar numa forma de tirar esse peso das costas dela.
* 

## Armazenamento e distribuição

Investigar deixar uma parte do artefato fora do zstd e baixá-la primeiro, permitindo começar a usá-la antes de receber o restante. Compressão e ordem de download ficam fora do desenho atual, que grava sem compressão.

Investigar agrupar unidades pequenas de corpos distintos para amortizar aberturas e reutilizar descritores ou mappings do mesmo arquivo ao longo das demandas do consumidor. Comparar o ganho de leitura com o custo de repacking e retenção de bytes obsoletos; o agrupamento não deve exigir carregar corpos não demandados. 

## Referências na imagem

Modelo híbrido: imediatos no caminho quente e uma tabela por imagem, no fim da própria alocação e lida por endereço relativo ao PC, para os alvos de operações, o `VM*` e as referências que só aparecem em slow paths, saídas de exceção e caudas de entrada. Troca os fixups por site por um slot por alvo distinto, mas exige formas novas no assembler (operando RIP-relativo no x86_64, `ldr` literal no ARM64 resolvido depois da compactação), um segundo tipo de fixup e slots privados para a chamada lenta de cada MathIC. Reabrir se o bench mostrar que aplicar fixups por site e colocar ilhas pesa na CPU da importação, do jeito que o limite do THREAD a conta.

Medir no ARM64 (Graviton2 e Graviton3 ou 4) o custo das formas fixas com os refinamentos do THREAD: censo de instruções por classe nos dois modos e microbenchmarks só com baseline, incluindo um caso com mais de 2 MB de outro código para tirar os desvios do alcance. Uma classe só ganha outra forma se o intervalo de confiança excluir 1% ou se o JetStream2 mudar mais de 0,5%.

## Corte

Depois, pensar em como fazer o corte sem parar as outras VMs: hoje ele suspende o worklist do JIT, que todas as VMs do processo compartilham.

## Harness and maintenance

About 180 fixture names in the UCB tests (`jitcacheUA7`, `jitcache-u8-attach-alpha` and the like, most from 1.4.2) are built from test IDs, which strings should not cite. Renaming them needs a build and a full run, since some checks match on those names.

A `.cache.replaced` that holds an unknown file blocks every later ConsumerProducer start until someone removes it. Replacement should move such a leftover aside instead of failing.

Scan images in twins builds for unrecorded immediates that fall inside the engine image. Such a reference passes every test, because our builds load the engine at its link-time address and the relocation check skips those targets; until 1.4.3 only the census review caught one, the tail-call operation pointer in `CallLinkInfo::emitFastPathImpl`.

`cb/tier-up-history.js` is not bench-safe: in `--bench`'s native copy its Consumer runs over an empty artifact while the script asserts imported state, so it fails there. Exclude it from the bench, or make its assertions depend on whether its bodies were imported.

Neither LLVM prefix holds a clang-format (`~/collo-local/tools/llvm-21`, `llvm-23`), so agents format with `/usr/bin/clang-format` 19.1.7, which disagrees with the tree on `{ }` spacing and short enums. Install the toolchain's clang-format (23.1.x once the bump lands) into its prefix from apt.llvm.org, as the rest of LLVM 23 came, before the next reformat, and name it in CLAUDE.md beside the prefix.

Measure whether native leaks vary between runs of one corpus script. A JSC stress test showed it on our build and on the pin alike: one or two of the single-character atoms `SmallStrings::initializeCommonStrings` creates. A corpus run and its oracle run are two processes, so such a leak could fail the harness's pairing of leaks although both are the engine's. Run each corpus script's `Off` run about 20 times and compare the leak sets; only if one varies, have the runner repeat the oracle run up to N times and accept a leak that some native sample reports, and say so in the harness sub-SPEC.

Propose `installationBoundFraction` and `installationBoundSmallestBody` from `PerformanceTests/JITCache/bound.ts` on a quiet release-local build, x86_64 here and ARM64 on the A1, by the rule Davi approved on 2026-10-12: the smallest body is the least installed image size whose excluded bodies hold under 5% of each workload's native compile CPU, and the fraction is the largest covered ratio once each outlier is fixed or explained, rounded up to the next 0.05. Show him, for candidate sizes from 0 to 4 KiB, the bodies covered, their share of native compile CPU and the p50, p99 and largest ratio, with the install steps of the worst body. Neither value enters HARNESS.md before he picks it.

The reference docs under skills/reference still describe webkitbun `2e2aa229` (Bun 1.4.2), while the tree runs `0c06faad`. File one report per doc in `report/`, as SKILL.md says, each checked against the 1.4.3 tree, whose native facts the SPECs recorded during the bump. basics.md: lazy FunctionExecutables and call data at link, UCB profiles allocated at the first baseline compile, `SourceCodeKey::operator==` comparing text again, the startup deferral scale in tier-up, weak-reference jettisons in the reoptimization back-off, run-once release and code returned to its payload. profiles.md: value-profile buckets with a lazy predictions array, `thresholdForValueProfilePredictions`, no predictions for code never optimized, the `this` profile moved into `CallSiteData`, lazy UCB copies and catch buffers, `iterator_close_check` modes, exact Int52 overflow, exit sites tallied from compiled stubs. inlinecaches.md: `LazyCallLinkInfo`, `CallSiteData`, the unlinked-call thunk and the executed-once state, no `doneLocation` on the handler IC, `ModuleVar`'s import slot, `LazyClosureVar`, scope metadata without a watchpoint set. baseline/compiler.md: the plan's `ensureValueAndArrayProfiles` and `updatePredictionsConcurrently`, the new templates, the VM sentinels and the tail-call operation pointer the code embeds, 8-byte value-profile slots. install.md: setup materializes lazy state first, OSR exit reads the molds, only door 1 allocates UCB profiles. locks.md: the fence that publishes `CallSiteData`, UCB profiles and the predictions array, and `UnlinkedCodeBlock::m_lock` around the expression-info decode. gc.md: Bun's idle-tagged collections and the aging snapshot on every look. bunpatches.md: aging, the two new host options and the startup deferral scale, the bytecode cache changes, run-once release and the idle shrink, several loaders per global object and private module UCBs, and a recount of its opening and its Details table.

SPEC-ics.md B2 asks the bench for each body's own `CallSiteData`s, its lazy call sites, those the consumer calls, and the allocations a `this` profile alone caused, but the bench report's `install` event (SPEC-integrator.harness.md section 9.2) has no field for them, so no bench can report B2 yet. Give the event those counts, or take them out of B2. The step 6 reader (~/collo-local/build/jitcache/archive/bump-2026-10/measure/b2-call-sites.ts, twins build) gives all but the called count: on 8 JetStream2 tests, 10,003 own `CallSiteData`s for 12,921 lazy sites, 9,708 of them for a `this` profile alone.

Decide whether a body imported without an image carries its LLInt counter progress. On the 1.4.2 beta (the A1, 20 to 40 consumers per configuration), consumers that armed the counter from zero (`jitCacheImportLLIntProgress` off) were equal or 0.5 to 1.5 ms faster on startup plus request 1 than consumers that carried it. On 1.4.3 (x86_64 release, `--compare-llint-progress`) they were again 0.1 to 2.8 ms faster. Branch `opt/llint-progress` (c9eeac107d4) drops the carried progress and the option; it changes THREAD Restoration and compact's L, so it waits for Davi.

The microbench of 2026-10-22 found `ucb/tdz-scale.mjs` slower after the reorganization, in every round: `delta`'s capture of bodies without an image (phase 3) went from 7.1 to 8.8 ms, its writer's reread 4 to 5% slower, and one body's install from about 3.4 to 13 µs. The TDZ digests moved from `CachedTypes.cpp` into `UCBKeys.cpp` in that range. Find the cause; the before and after release snapshots are in `~/collo-local/build/jitcache-opt/snapshots/regress-{before,after}`. In a Consumer run of the script, a few requests spend 5 to 95 µs in their Records part at nearly the same positions in both builds, with no collection in the run, so a one-time cost such as a table growing lands on whichever body crosses it; one of them moved to the neighbouring body between the builds.

The macro bench cannot resolve a few percent on a case whose producers vary. Preact varied's +4% on the A1 came from one function, `N2`, which tiers up to DFG at request 3 and to FTL inside the 500-call window in about two-thirds of producer runs, both before and after the reorganization (54 and 52 of 80). With three producers per run, one producer more in that mode moves the median by 2 ms. Give each comparison more producers, or split it by mode. React fixed (+2.4%) and Preact fixed (+1.8%) on requests 2 to 35 stay unexplained at that resolution. The runs are under `~/collo-local/build/jitcache/organization-regression-20261022`.

Check which ways a UCB or a CB can die still go untested, and test them. `ucb/drop-and-reimport.js` covers a UCB that `Bun.gc(true)` drops, with its registry entries and its re-import; `ucb/run-once-capture.js` covers a body whose CB died before `delta`, which `delta` reaches through the registry; and `integrator/runner/events-dead-body.js` checks that the body-event dump keeps the counts of a UCB that aged out under `useUnlinkedCodeBlockJettisoning`.

## Code organization

Twins-only code still sits in production files: 15 `ENABLE(JITCACHE_TWINS)` guards in `store/ArtifactWriter.cpp`, 11 in `image/ImageRecord.cpp`, 8 in `image/ImageCapture.cpp`. The install glue's checks already moved to `glue/InstallTwins.cpp`. The reorganization's plan moved each twins block longer than about 20 lines into its part's twins file behind a one-line guarded call, and split long procedures where a phase boundary carries an invariant (the install function into a fallible prepare that writes nothing and an infallible commit; `prepareImage`, `importBody`, `seedDecoded`, `attachLive`, `commitCapture` and `compact` along their steps). That landed only where the file moves made it fall out. Per-lane sub-namespaces, which only the ICs lane has, were left for the next SPEC compaction.

Still to do from the October reorganization. The CB lane's capture should write into spans the glue sizes and charges, and its twin check should return values the glue reports, as the ICs lane's do. The Image twin check should take its subject from the glue instead of `VMState`. The UCB lane should get a session header with only the members it uses, and the integrator-only accessors should leave the class the lanes see. In the engine, one `JITCache::Access` friend template should replace the lanes' accessors in about 15 engine headers, and each owner should get its own hook header, with one glue call where several owners observe one engine event.

The workflows still assume the layout from before that reorganization: `thread-implement.js` and `thread-bench.js` need a build snapshot that no longer exists, `thread-implement.js` names an LLVM 21 symbolizer and runs a removed UCB self-test, and `thread-bench.js` writes to `docs/JitCache/results/` instead of `PerformanceTests/JITCache/`. Update them before their next run, and let the implementation workflow call the runner's `--ladder`.

## Optimization branches

Each branch is a measured proof of concept on `3ad874d59c6` and needs a rebase onto main, a test run and the same benches before it lands. Their numbers came from the bench before its timers stopped putting clock reads inside the bound's spans, which took the bound's p50 from about 1.49 to 1.00 on x86 and from 1.09 to 0.84 on the A1, so their effects on the bound must be measured again.

- `opt/codemap` (f9d587ed725): the code map is built in one allocation. The rest of `prepareImage` is 27 to 29% faster; the whole bound does not move beyond noise.
- `opt/import-probe` (d78ff0ae303): a three-stream CRC32C, the body validated in place and one index lookup per import. The lookup per opened body is 21% faster; misses cost about 15 ns more.
- `opt/commit-io` (b9c12e82c79): one `pwrite` per commit and no `fstatat`. A commit is 24% faster.
- `opt/compact-index` (f4ad422f823): 8-byte index slots. The consumer's index table drops from 3.9 MB to 256 KB on `many-bodies.js`, and building it is about 90% faster; a ConsumerProducer lookup costs about 40 ns more.
- `opt/llint-progress` (c9eeac107d4): see the LLInt-progress entry above.

## Porte para macOS (baixa prioridade)

Hoje o JITCache compila onde o Bun compila, com o código só de Linux atrás de guardas, e `start` rejeita fora do Linux. O porte precisa do UUID do Mach-O como build ID, do vetor de CPU via sysctl, de um índice sem inotify, de `MAP_JIT` e W^X ao instalar a imagem, e do posicionamento de endereços do harness.

# Not planned (but might add)

Callback C++ para notificação assíncrona de falhas do JITCache. A API atual usa resultados das chamadas, estado consultável da sessão e diagnóstico nativo; não inclui callbacks nem sua coordenação de thread/reentrada.

Salvar mais de um CB por corpo para melhor suporte a multiplos realms

Transportar stubs de IC compilados. Hoje eles ficam de fora, e o consumidor os compila nativamente quando a recipe liga. No M3 eram 0,89% dos casos de IC baseline em 57 workloads do JetStream2, no máximo 5,13% num workload pequeno (gaussian-blur, 39 casos) e 3,35% entre os grandes (espree), com compilação mediana de 2,1 µs e no máximo 0,70 ms por workload. Reabrir se o bench gate mostrar essas compilações na latência da primeira requisição de workloads Bun, onde leituras de `globalThis` pelo `JSGlobalProxy` e `Buffer.length` podem ser mais frequentes que no shell do jsc. Nesse caso, atacar primeiro as transições com condição checada, que são 85% das compilações, talvez com um thunk que leia as condições dos dados do handler em vez de imagens de stub; é mudança no motor e ainda não foi verificada.

Analisar formas de passar thunks e handlers mantendo a performance do resultado final e chegando nele mais rápido que recompilar. Hoje a imagem leva só a chave nativa de cada suporte, e o consumidor gera o que falta pelo lookup nativo, como no primeiro uso da VM.

Prepare imported images on the JIT worker threads. Research of 2026-10-14 (1.4.2, the A1): the image never depends on the CB, so once the request point has resolved the UCB's values, a worker can allocate, copy, patch and flush it without reading a cell; JITCache's `SupportTargetTable` would need a lock. For 97.5% of imports the request point and the body's first CB fall inside one `prepareForExecutionImpl` call, which leaves a worker 5 to 20 µs. On one CPU it only adds 5 to 10 µs per body, so it stays off there; in this fork `WTF::numberOfProcessorCores()` counts the affinity mask and the cgroup quota, so it reads 1 under `taskset`. On two CPUs a hono-sized first call (115 imports, 4.6 ms of preparation) would move at most 1 ms off the main thread with a plain hand-off, and 1.2 to 2.9 ms if the VM thread shares the work with the worker. It changes THREAD's opening, which runs JITCache's steps on the VM thread, and the installation bound's accounting, so it is Davi's. A proof of concept of the plain hand-off, never built, is on branch `poc/worker-prep` in `~/collo-local/build/jitcache-poc/webkit`, on 1.4.2; its first measurement would be the worker's wake latency on the A1.

Transportar a distribuição de callees, ou fechar de outro jeito a janela em que a primeira compilação DFG do consumidor começa antes de os call sites quentes ligarem. Hoje a distribuição não viaja e o custo da janela é aceito; um status vazio impede inline mesmo de callee constante. O bench mede se vale reabrir (seção abaixo). Se muitos corpos quentes perderem inlining, a alternativa é o portão do item seguinte.

Portão antes da primeira compilação DFG de um CB importado, como refinamento da regra que tira o progresso do contador dos corpos polimórficos. Hoje um corpo com IC de propriedade cujo registro capturado lista dois ou mais casos, fora dobras megamórficas, não leva progresso do contador baseline: uma invocação do consumidor só reconstrói os casos que encontra, a compilação DFG no piso especularia só neles, e as saídas BadCache seguintes entrariam no perfil de saídas e deixariam lenta toda compilação posterior nesses sites. O portão deixaria esses corpos levarem o progresso, e também cobriria a primeira invocação dos corpos que fazem loop ou recursão, que o piso não cobre. A forma estudada fica em `operationOptimize`, só para CBs com estado importado, depois do limiar e dos adiamentos nativos e antes de `shouldOptimizeNowFromBaseline`: cada site pendente (IC com menos casos que o produtor capturou, call site que o produtor tinha ligado, escopo que ele tinha promovido e o link do consumidor deixou sem resolver) tem um bit que cai quando o site alcança o estado do produtor, e enquanto houver bit o portão re-arma uma fatia curta preservando `count()` e `m_activeThreshold`, como a cauda de `ExecutionCounter::setThreshold`, até gastar um orçamento local de pontos. Reabrir se o bench mostrar que corpos polimórficos chegam ao DFG mensuravelmente mais tarde que no run aquecido do produtor, ou jettison de corpos importados por `ForceOSRExit`. Antes de reabrir, medir quantos CBs o orçamento liberaria com sites ainda pendentes, porque sites que o produtor aqueceu e o consumidor nunca alcança seguram o portão até o fim do orçamento.

Transportar os caches de metadata que guardam células: caches de propriedade do LLInt, a Structure de `to_this` e os callees de `create_this` e parentes. Hoje começam como o link nativo deixa. Num CB nascido em baseline, só a compilação DFG que cai na janela antes de o IC ligar a recipe perde algo: o status lê sem informação, e um `put_by_id` ainda planta `ForceOSRExit`. A forma estudada resolvia a Structure a partir de uma raiz do realm por transições de adição de propriedade.

Transportar as previsões dos buffers de `catch`, hoje omitidas. O buffer nasce no primeiro catch, a partir da liveness da UCB, e cria o rare data da CB. Semeá-lo exigiria alocá-lo cedo com a lista de operandos do produtor, por uma entrada nativa anterior ao tipo de JIT, porque o alocador público exige CB baseline e um seed depois de `installCode` corre contra o primeiro dreno do marker. Reabrir se o bench mostrar compilações DFG de corpos importados que leem um `catch` antes do primeiro catch do consumidor e saem por falta de predição.

Bun 1.4.3 changes the entry above: both the value profiles of `catch` entries and the mark that a catch ran without its buffer should travel later. In 1.4.3 a catch gets its buffer when its function first crosses the DFG threshold (`useLazyCatchLiveness`), a native point before the DFG compile, so seeding the buffer's predictions there no longer needs the early allocation the entry above describes. The CodeBlock's flag (`m_hasCatchThatExecutedWithoutBuffer`) cannot travel as captured: after the producer's DFG compile the flag is clear while each catch's `hasExecutedWithoutBuffer` bit stays set, so a restore would derive the flag from the bits. Once catch state travels, `useLazyCatchLiveness` must be fixed on again, because the marks mean that a catch ran before its buffer existed only under it.




otimizar ics

Voltar em execeções tipo RepatchingPropertyInlineCache quando for implementar ics. 


**Producer limit: saturate instead of ending production.** Today a charge past `producerLimitBytes` is a recording fault that ends production for good, so a host can only use the limit as a large safety net. Instead, a compilation whose recording would pass the limit is not recorded: it runs natively and is not captured from that code. A capture whose buffers do not fit is skipped, and production goes on. Refusals are not sticky, so memory freed when code dies is reused. `status` reports the peak and the counts of unrecorded compilations and skipped captures. Startup code compiles first, so a saturated producer still captures what matters most for a cold start, and Bun can get a finite default from the bench. The change reuses `ProducerBudget` and the Image recorder's unrecordable outcome; it changes THREAD Failures, the refusal paths and their tests. Later, if records dominate, evict by compact's (L + P) / B score.

ajustar isso:
config.producerLimitBytes = SIZE_MAX;                // --jitcache-max-memory; unlimited, as in Bun

e adicionar no readme. (so pra host)