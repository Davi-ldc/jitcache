# First priority

Make body identity function-relative, so that an app whose source changed still imports every function that did not change. Today a root is identified by a digest of its whole text and each nested function by its parent's key, table and index, so one edit makes every body of a bundle miss; for Bun 1.4.3 a changed app turns JITCache off with a warning instead. A function's UCB does not depend on where the function sits (`CodeBlock::expressionInfoForBytecodeIndex` adds the function's `sourceOffset()` to the positions its UCB stores), and the holder digest of its UFE already covers everything else its generation reads. A key made of the function's own text digest and a holder digest without absolute positions would therefore let unchanged functions keep importing across deploys. Measure first how many functions keep identical text when a bench app is edited a little and rebuilt with `bun build`, with and without `--minify`. Two identical functions would share one body, which baked facts and the richness rule keep sound.

Support multithreading and workers later. A `child_process.fork` child that runs another entry file inherits the parent's `--jitcache` flags but gets another app digest, so for Bun 1.4.3 it is refused (Consumer parent) or turned off by the producer lock (ConsumerProducer parent) and runs without JITCache. Function-relative identity removes the app gate; until then Bun could pass the parent's digest to forked children.

# Cool ideas
* analisar somar profiles ao invés de ficar com o melhor.

## Compact

Investigar a fração de CPU atribuída ao corpo em relação à CPU total da execução como um sinal adicional para a heurística de `compact`, separando trabalho de CPU de espera por I/O. A coleta, sua atribuição e seu overhead precisam ser avaliados antes de entrar no desenho; por enquanto, a poda usa apenas o aquecimento restaurável e os bytes efetivamente recuperáveis.

Analisar a possibilidade de `compact` comprimir strings e outros dados duplicados para reduzir o tamanho do artefato. O desenho atual aceita duplicação entre arquivos de corpos e os mantém autocontidos, sem compressão.

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

## Strict

Otimizar o strict. Hoje ele valida a estrutura inteira do que o JITCache lê e as suposições que ele faz, por isso é lento e fica desligado por padrão; o modo normal só confere a integridade (header, chaves e checksums). Todo teste roda com ele ligado, e o bench mede o padrão.

O decode estrito de um core deve falhar no segundo alcance de um registro que ele lê no lugar (constantes, `CachedOptional` e o rare data que `CachedVariableEnvironment` e `CachedSymbolTable` leem direto), como já falha para os registros do cache de offsets. Hoje esses registros decodificam a cada alcance, como no decode nativo, e um core forjado que nomeia um deles de muitos slots pode esgotar a memória dentro do `DeferGC` do decode.

O S3 do CB ainda rejeita um caso raro: um progresso negativo commitado ao lado de um total grande e restaurado ao lado de um total menor falha numa recaptura estrita antes de o corpo rodar de novo. Ele precisa de um teto de slice fora do padrão, acima de 2^24, e de uma chamada que lance entre a instalação e a entrada. Fechá-lo pede que `finishCounter` restaure `max(P, 0)`, o que muda o I7 e a expectativa `counter-progress` do twin.

## Harness and maintenance

About 180 fixture names in the UCB tests (`jitcacheUA7`, `jitcache-u8-attach-alpha` and the like, most from 1.4.2) are built from test IDs, which strings should not cite. Renaming them needs a build and a full run, since some checks match on those names.

A `.cache.replaced` that holds an unknown file blocks every later ConsumerProducer start until someone removes it. Replacement should move such a leftover aside instead of failing.

Scan images in twins builds for unrecorded immediates that fall inside the engine image. Such a reference passes every test, because our builds load the engine at its link-time address and the relocation check skips those targets; until 1.4.3 only the census review caught one, the tail-call operation pointer in `CallLinkInfo::emitFastPathImpl`.

`cb/tier-up-history.js` is not bench-safe: in `--bench`'s native copy its Consumer runs over an empty artifact while the script asserts imported state, so it fails there. Exclude it from the bench, or make its assertions depend on whether its bodies were imported.

Neither LLVM prefix holds a clang-format (`~/collo-local/tools/llvm-21`, `llvm-23`), so agents format with `/usr/bin/clang-format` 19.1.7, which disagrees with the tree on `{ }` spacing and short enums. Install the toolchain's clang-format (23.1.x once the bump lands) into its prefix from apt.llvm.org, as the rest of LLVM 23 came, before the next reformat, and name it in CLAUDE.md beside the prefix.

Measure whether native leaks vary between runs of one corpus script. A JSC stress test showed it on our build and on the pin alike: one or two of the single-character atoms `SmallStrings::initializeCommonStrings` creates. A corpus run and its oracle run are two processes, so such a leak could fail the harness's pairing of leaks although both are the engine's. Run each corpus script's `Off` run about 20 times and compare the leak sets; only if one varies, have the runner repeat the oracle run up to N times and accept a leak that some native sample reports, and say so in the harness sub-SPEC.

Propose `installationBoundFraction` and `installationBoundSmallestBody` from `PerformanceTests/JITCache/bound.ts` on a quiet release-local build, x86_64 here and ARM64 on the A1, by the rule Davi approved on 2026-10-12: the smallest body is the least installed image size whose excluded bodies hold under 5% of each workload's native compile CPU, and the fraction is the largest covered ratio once each outlier is fixed or explained, rounded up to the next 0.05. Show him, for candidate sizes from 0 to 4 KiB, the bodies covered, their share of native compile CPU and the p50, p99 and largest ratio, with the install steps of the worst body. Neither value enters HARNESS.md before he picks it.

## Porte para macOS (baixa prioridade)

Hoje o JITCache compila onde o Bun compila, com o código só de Linux atrás de guardas, e `start` rejeita fora do Linux. O porte precisa do UUID do Mach-O como build ID, do vetor de CPU via sysctl, de um índice sem inotify, de `MAP_JIT` e W^X ao instalar a imagem, e do posicionamento de endereços do harness.

# Not planned (but might add)

Callback C++ para notificação assíncrona de falhas do JITCache. A API atual usa resultados das chamadas, estado consultável da sessão e diagnóstico nativo; não inclui callbacks nem sua coordenação de thread/reentrada.

Salvar mais de um CB por corpo para melhor suporte a multiplos realms

Transportar stubs de IC compilados. Hoje eles ficam de fora, e o consumidor os compila nativamente quando a recipe liga. No M3 eram 0,89% dos casos de IC baseline em 57 workloads do JetStream2, no máximo 5,13% num workload pequeno (gaussian-blur, 39 casos) e 3,35% entre os grandes (espree), com compilação mediana de 2,1 µs e no máximo 0,70 ms por workload. Reabrir se o bench gate mostrar essas compilações na latência da primeira requisição de workloads Bun, onde leituras de `globalThis` pelo `JSGlobalProxy` e `Buffer.length` podem ser mais frequentes que no shell do jsc. Nesse caso, atacar primeiro as transições com condição checada, que são 85% das compilações, talvez com um thunk que leia as condições dos dados do handler em vez de imagens de stub; é mudança no motor e ainda não foi verificada.

Analisar formas de passar thunks e handlers mantendo a performance do resultado final e chegando nele mais rápido que recompilar. Hoje a imagem leva só a chave nativa de cada suporte, e o consumidor gera o que falta pelo lookup nativo, como no primeiro uso da VM.

Transportar a distribuição de callees, ou fechar de outro jeito a janela em que a primeira compilação DFG do consumidor começa antes de os call sites quentes ligarem. Hoje a distribuição não viaja e o custo da janela é aceito; um status vazio impede inline mesmo de callee constante. O bench mede se vale reabrir (seção abaixo). Se muitos corpos quentes perderem inlining, a alternativa é o portão do item seguinte.

Portão antes da primeira compilação DFG de um CB importado, como refinamento da regra que tira o progresso do contador dos corpos polimórficos. Hoje um corpo com IC de propriedade cujo registro capturado lista dois ou mais casos, fora dobras megamórficas, não leva progresso do contador baseline: uma invocação do consumidor só reconstrói os casos que encontra, a compilação DFG no piso especularia só neles, e as saídas BadCache seguintes entrariam no perfil de saídas e deixariam lenta toda compilação posterior nesses sites. O portão deixaria esses corpos levarem o progresso, e também cobriria a primeira invocação dos corpos que fazem loop ou recursão, que o piso não cobre. A forma estudada fica em `operationOptimize`, só para CBs com estado importado, depois do limiar e dos adiamentos nativos e antes de `shouldOptimizeNowFromBaseline`: cada site pendente (IC com menos casos que o produtor capturou, call site que o produtor tinha ligado, escopo que ele tinha promovido e o link do consumidor deixou sem resolver) tem um bit que cai quando o site alcança o estado do produtor, e enquanto houver bit o portão re-arma uma fatia curta preservando `count()` e `m_activeThreshold`, como a cauda de `ExecutionCounter::setThreshold`, até gastar um orçamento local de pontos. Reabrir se o bench mostrar que corpos polimórficos chegam ao DFG mensuravelmente mais tarde que no run aquecido do produtor, ou jettison de corpos importados por `ForceOSRExit`. Antes de reabrir, medir quantos CBs o orçamento liberaria com sites ainda pendentes, porque sites que o produtor aqueceu e o consumidor nunca alcança seguram o portão até o fim do orçamento.

Transportar os caches de metadata que guardam células: caches de propriedade do LLInt, a Structure de `to_this` e os callees de `create_this` e parentes. Hoje começam como o link nativo deixa. Num CB nascido em baseline, só a compilação DFG que cai na janela antes de o IC ligar a recipe perde algo: o status lê sem informação, e um `put_by_id` ainda planta `ForceOSRExit`. A forma estudada resolvia a Structure a partir de uma raiz do realm por transições de adição de propriedade.

Transportar as previsões dos buffers de `catch`, hoje omitidas. O buffer nasce no primeiro catch, a partir da liveness da UCB, e cria o rare data da CB. Semeá-lo exigiria alocá-lo cedo com a lista de operandos do produtor, por uma entrada nativa anterior ao tipo de JIT, porque o alocador público exige CB baseline e um seed depois de `installCode` corre contra o primeiro dreno do marker. Reabrir se o bench mostrar compilações DFG de corpos importados que leem um `catch` antes do primeiro catch do consumidor e saem por falta de predição.

Bun 1.4.3 changes the entry above: both the value profiles of `catch` entries and the mark that a catch ran without its buffer should travel later. In 1.4.3 a catch gets its buffer when its function first crosses the DFG threshold (`useLazyCatchLiveness`), a native point before the DFG compile, so seeding the buffer's predictions there no longer needs the early allocation the entry above describes. The CodeBlock's flag (`m_hasCatchThatExecutedWithoutBuffer`) cannot travel as captured: after the producer's DFG compile the flag is clear while each catch's `hasExecutedWithoutBuffer` bit stays set, so a restore would derive the flag from the bits. Once catch state travels, `useLazyCatchLiveness` must be fixed on again, because the marks mean that a catch ran before its buffer existed only under it.




otimizar ics

Voltar em execeções tipo RepatchingPropertyInlineCache quando for implementar ics. 


**Producer limit: saturate instead of ending production.** Today a charge past `producerLimitBytes` is a recording fault that ends production for good, so a host can only use the limit as a large safety net. Instead, a compilation whose recording would pass the limit is not recorded: it runs natively and is not captured from that code. A capture whose buffers do not fit is skipped, and production goes on. Refusals are not sticky, so memory freed when code dies is reused. `status` reports the peak and the counts of unrecorded compilations and skipped captures. Startup code compiles first, so a saturated producer still captures what matters most for a cold start, and Bun can get a finite default from the bench. The change reuses `ProducerBudget` and the Image recorder's unrecordable outcome; it changes THREAD Failures, the refusal paths and their tests. Later, if records dominate, evict by compact's (L + P) / B score.

Não li o `todo.md` nesta sessão, então talvez seja preciso ajustar ao formato das outras entradas.


ajustar isso:
config.producerLimitBytes = SIZE_MAX;                // --jitcache-max-memory; unlimited, as in Bun

e adicionar no readme. (so pra host)