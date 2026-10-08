# bun patches

a ideia aqui é documentar um pouco mais a fundo o que que o bun ta mudando que nao foi upstreamado e por que faz sentido pra eles nao pro webkit. No geral, é bem compativel com o jsc e eles mergeiam quase toda semana, então faz sentido pra gente trabalhar em cima dele.

PS: dos 2.152 commits que separam `WebKit/WebKit@0d58b764f3` de `oven-sh/WebKit@2e2aa2290f` 333 são merges e história antiga refeita, mais da metade do volume é troca de `#include`. Na prática, oq sobra mora em `runtime/`, `heap/`, no cache de bytecode e no diretório novo `ffi/`; o baseline JIT tem um único sítio guardado, um campo novo em `BaselineJITCode`, e nenhum emissor mudou. 

## Aging de código (PR #557, `CodeBlock::shouldJettisonDueToOldAge`):

O limite de tempo em que um CB tá protegido no upstream (5 s LLInt, 15 s baseline) pode fazer um serverless ficar deletando CBs entre chamadas e refazendo a linkagem e os perfis — não necessariamente recompilando, porque o código baseline pode continuar no UCB. Por isso, sob `USE(BUN_JSC_ADDITIONS)` e com `useExecutionCountForCodeBlockAging` ligada, na marcação, depois que a proteção acabou, o GC lê o contador do tier (`llintExecuteCounter().count()` do UCB, `executeCounter().count()` do `BaselineJITData`, `tierUpCounter().count()` do `DFG::JITData`) e compara com `m_previousCounter`. Se mudou (iteração em loop também conta), atualiza a amostra e põe `m_creationTime` em `agora + 2 × TTL`, o que faz `timeSinceCreation() < TTL` valer por mais `3 × TTL` (`codeBlockAgingLeaseMultiplier`, padrão 3). São mais 15 s no LLInt, 45 s no baseline e 60 s no DFG a partir daquela checagem; depois disso ele pode renovar de novo. Se não mudou, perde essa proteção, mas outra referência ainda pode mantê-lo vivo. No LLInt o contador é compartilhado pelo UCB, então outro CB daquele corpo rodando também conta.

FTL não tem contador de tier-up, e DFG pode ser compilado sem as instruções que incrementam ele depois de uma falha de compilação FTL. Esses casos não podem envelhecer por esse sistema, então só podem morrer por idade numa coleta que o Bun marcou como idle, se tiverem sido criados há pelo menos 30 segundos e a VM estiver há 30 segundos sem dar sinal. Sinal aqui é quando uma coleta encontra mais de 1 MiB alocado pela VM inteira, acumulado desde a última atualização; passando disso, zera o acumulado e renova o carimbo. Sem essas condições, não envelhecem. Quando um otimizado envelhece, suas referências fracas vivas deixam de salvá-lo e ele deixa de proteger o baseline alternativo; ambos ainda podem sobreviver por outras referências. Essas mudanças também alteram o comportamento do `BaselineJITCode` no UCB: agora o slot é limpo no fim de um full por `Heap::releaseUnusedSharedBaselineCode` se só o UCB ainda segura o código e o último CB dono morreu há mais de 45 s. O `jettison` por idade também pode limpar esse slot, sem esperar os 45 s; a memória executável volta quando cai a última referência.

## Fronteira nativa

O segundo bloco corta cópia e redescoberta na travessia JS→nativo: iterador de `JSRopeString` que entrega cada pedaço sem achatar, `ExternalStringImpl` com um ponteiro a mais e strings estáticas, typed arrays pelo DOMJIT, e a remoção de boa parte dos `JSLockHolder` da C API porque, nas palavras deles, o lock é tratado pelo bundler. Na interface com o embedder, `Config::disableEnvironmentOptions()` pula a leitura de `JSC_*` do ambiente, `Interpreter::executeProgram` aceita um `UnlinkedProgramCodeBlock` que o embedder já tem, e `VM::addTerminationDeadline` é um prazo de relógio de parede por chamada, distinto do `Watchdog`.

O DFG e o FTL usam o `BufferAccessorRegistry` pra transformar chamadas como `Buffer.prototype.readInt32LE` em operações de leitura ou escrita com testes de tipo e limites, usando os profiles e exits já coletados. Já o `bun:ffi` pode gerar um stub de entrada por `JSFFIFunction`, especializado pela assinatura fornecida, com testes de aridade e conversão; quando um teste falha, a chamada segue pelo caminho genérico. Esse stub reaproveita um thunk de invocação compartilhado por assinatura no processo (`SignatureRegistry::intern` e `Signature::invokeThunk`, em `ffi/FFISignature.cpp`). Quando gerado, o stub vira a entrada de chamada do `NativeExecutable`, que o `CallLinkInfo` guarda como destino (`JSFFIFunction::create`, em `ffi/JSFFIFunction.cpp`).

## Contexto assíncrono e módulos

O `AsyncLocalStorage` exige que um job carregue o contexto de quem o agendou. O fork guarda esse contexto num `InternalFieldTuple` (célula de dois campos com `JSType` próprio) pendurado em `JSGlobalObject::m_asyncContextData`, captura-o ao enfileirar reactions de promise, e o `AsyncContextSwapScope` instala e restaura em volta de cada job; até o embedder chamar `VM::setAsyncContextTrackingEnabled` tudo se reduz a um teste de flag, e o DFG/FTL abandonam o fast path de `PerformPromiseThenOneHandler` quando há contexto ativo. Em módulos, `VM::m_synchronousModuleQueue` faz `require(esm)` carregar, linkar e avaliar sem ceder a microtasks do usuário e sem recursão em C++ proporcional ao número de módulos.

## Arranque

O objetivo declarado é que um `bun build --compile --bytecode` toque só as páginas do que decodifica. O cache de bytecode foi reescrito nessa direção: layout por região com os arrays do bloco na frente, offsets de 32 bits, strings deduplicadas com átomos curtos inline, tabela de metadata transportada como contagem por opcode e recomposta com os tamanhos locais (`UnlinkedMetadataTable::expandSteps`), formato portátil e determinístico. A mudança estrutural é `useBorrowedBytecodeFromCache`: um `InstructionStream` decodificado de um payload que o embedder marcou como persistente (`CachePayload::setIsPersistent`) aponta pros bytes mapeados em vez de copiá-los, o `InstructionStream` ganhou um modo `Borrow` e lê tudo por `span`, e o `UnlinkedCodeBlock` conta os bytes emprestados pro pacing do GC como se tivesse gerado o bytecode; a economia de cópia não reduz essa conta. Sem embedder marcando payload persistente nada disso acontece. Fora do cache: tabela de átomos dimensionada pra uma VM do Bun, `WebAssembly` lazy e prewarm de timezone pulado.

## Memória e coletor

`WTF::availableMemory()` conta o limite do cgroup (`uv_get_constrained_memory()`, em `wtf/uv_get_constrained_memory.cpp`); a heurística de crescimento perde o `GrowthMode` e o rate limiting, e o um terço que decide um full depois de um eden vira a option `minEdenToOldGenerationRatio`; `Heap::setInitialAllocationBudget` deixa o embedder adiar a primeira coleta quando vai montar um grafo grande e inteiramente vivo. Depois de uma varredura pós-full, `MarkedBlock::Handle::decommitUnusedPages` devolve ao SO as páginas de um bloco sem célula viva, exceto nos espaços de `Structure`, porque um destrutor pode ler a `Structure` morta. As reservas saem do THP por mapeamento e ganham `MADV_DONTFORK` em `OSAllocatorPOSIX.cpp` (inferência: o Bun faz fork+exec de filhos e não quer copiar reservas de gigabytes). Tudo isso é política de processo servidor de vida longa, o que explica ficar no fork. Na receita local do Bun, perfis sem ASAN ligam `USE_MIMALLOC` e `USE_EXTERNAL_MIMALLOC`; com ASAN, o helper preserva os defaults do motor. Nosso build segue o perfil escolhido, sem assumir libpas em todas as configurações.

## Yarr, BigInt e plataforma

Trabalho algorítmico sem relação com o embedder: JIT de lookbehind, despacho de alternação e Boyer-Moore no Yarr; divisão de Burnikel-Ziegler e Barrett, `toString` por divisão e conquista e `maxLengthBits` de 2³⁰ em `JSBigInt`. Nada disso entra na imagem baseline. O resto é o fork como produto: unwind info sobre o pool JIT no Windows (os +251 de `ExecutableAllocator.cpp` são só isso), builds cruzados e LTO, ARM64 com página de 64K, `$vm` e disassembler do JIT compilados fora do release (`BUN_ENABLE_JSDOLLARVM`, `BUN_ENABLE_JIT_DISASSEMBLER`), teto de 1 MB pro buffer de assembler cacheado por thread, e domínios de inspector do Bun. Correções de motor que ficaram no fork: watchpoints de stub de IC sobrevivendo às células das condições (`GCAwareJITStubRoutine.cpp`), `JSArray::setLength` limpando slots vivos com `memset` e correndo com o marcador, e o `InvalidationPoint` de `op_check_traps` mantido em todo loop. Duas `RELEASE_ASSERT` de `hasAccessBit` em `Heap::stopIfNecessarySlow` estão comentadas sem justificativa no código.

## Compatibilidade com Node

Pra entregar a mesma semântica observável do Node, a `VM` ganha callbacks pra formatar `Error.prototype.stack` (`onComputeErrorInfo`), acrescentar frames assíncronos (`onAppendStackTrace`, chamado no fim de `Interpreter::getStackTrace`) e, em perfis de CPU, resolver linha e coluna por sourcemap (`computeLineColumnWithSourcemap`, que o JSC só chama em `SamplingProfiler::stackTracesAsJSON`); `ErrorInstance::captureStackTrace` reescreve o stack guardado; `JSGlobalObject::overridenDateNow` é honrado por `Date.now()`, pelo intrínseco do DFG/FTL e por `Temporal.Now`; `useV8DateParser` troca o parser de datas pelo do V8; a mensagem de `ReferenceError` e o `Function.prototype.toString` de nativas imitam o V8; e há um heap snapshot no formato do V8 (`heap/BunV8HeapSnapshotBuilder.cpp`). blablabla

## Condições

Compilar com os perfis do Bun não inicializa automaticamente a VM pelo host Bun. Três regimes importam: o incondicional vale pra todo mundo, o guardado por `USE_BUN_JSC_ADDITIONS` está compilado, e o que depende de uma chamada do embedder fica inerte até alguém fazê-la.

| tema | regime |
|---|---|
| heurística do heap, decommit de páginas, THP e `MADV_DONTFORK`, teto do buffer de assembler, locks da C API, Yarr e BigInt, correções de motor | incondicional |
| aging por contador e liberação do baseline compartilhado, `gcMaxHeapSize` reposicionado, tabela de átomos menor | guardado, ativo no nosso build |
| aging de FTL por quietude, callbacks de erro e stack, `overridenDateNow`, contexto assíncrono, `setInitialAllocationBudget`, bytecode emprestado do cache, FFI, buffer accessors | depende de o embedder chamar, registrar ou marcar |
| mimalloc, `$vm` e disassembler, unwind do Windows, 64K pages | CMake/perfil ou plataforma; não assumir o mesmo alocador e instrumentação entre debug/ASAN e release |

# Detalhes 

| medida | valor |
|---|---|
| arquivos alterados em JSC + WTF + bmalloc | 1.191 (+39,5k / −6,2k linhas) |
| arquivos com mudança que não é `#include` | 516 |
| sítios `USE(BUN_JSC_ADDITIONS)` | 473, em 165 arquivos (linhas `#if`/`#elif` que testam a macro, contadas com grep em `.cpp`, `.h`, `.c` e `.mm` de JSC, WTF e bmalloc; `#endif` de fechamento e comentários não contam) |
| por diretório | `runtime/` 297 · WTF 31 · `ffi/` 30 · `dfg/` 24 · `heap/` 23 · `bytecode/` 11 · `parser/` 10 · `ftl/` 8 · `interpreter/` 7 · `bytecompiler/` 3 · `jit/` 1 · outros 28 (`jsc.cpp`, `tools/`, `inspector/`, `wasm/`, `debugger/`) |