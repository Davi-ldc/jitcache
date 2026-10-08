# Abstract

A ideia de compilar baseline é cortar o overhead de despachar `llint_op_*` a cada bytecode e emitir caminho rápido em cima do que os perfis tiverem observado. Repara que não tem OSR exit aqui: o teste que falha salta para o caminho lento e a execução segue. Dispara quando o contador de tier-up cruza o limiar, ou, com o LLInt desligado, no nascimento de um `CodeBlock` cujo `UnlinkedCodeBlock` ainda não guarda um artefato compartilhado. Tudo acontece sem IR no meio, sobre dois objetos de programa, o UCB e o CB linkado, em quatro etapas: a entrada da função, os caminhos rápidos numa passada única sobre o bytecode, a resolução dos saltos e o bloco dos caminhos lentos. O link tem outras quatro: a cópia com compactação, as tabelas laterais, o mapa de bytecode e a finalização. No final sobra o artefato que a instalação recebe.

Assim que uma thread pega o plano, a do pool, ou a que pediu (com `useConcurrentJIT` desligado, e sempre no nascimento de um `CodeBlock` com o LLInt desligado, pelo `JIT::compileSync`), ela drena os perfis de valor e abre um safepoint. Na do pool o safepoint solta o `m_rightToRun`, então ela compila com o coletor andando, e para nada que ela lê ser coletado no meio, o plano faz o GC marcar o `CodeBlock` como vivo. Na que pediu o safepoint não solta nada: essa thread mantém o acesso ao heap, então o coletor não consegue parar o mundo antes de a compilação acabar. PS: tem um compilador novo em `Options::useLOLJIT()`, desligado por padrão.

Além do UCB e do CB linkado, ela lê a `VM`, de onde sai o limite da stack que a entrada da função compara, e as `Options`, que moram no `JSCConfig` do processo — graváveis até a primeira `VM` existir e trancadas por `mprotect` depois dela. Algumas mudam o código emitido, quase sempre acrescentando instrução: `traceBaselineJITExecution` põe um `probeDebug` por bytecode e mais um em cada caminho lento, `forceGCSlowPaths` troca o caminho rápido de alocação por um salto, e a `useJITDebugAssertions` acrescenta uma checagem com `breakpoint()` ao caminho de propriedade global do `put_to_scope` (mesmo em release).

`useDFGJIT` e `maximumOptimizationCandidateBytecodeCost` fazem isso pela via do `capabilityLevel()` descrita adiante. `useLLIntICs` age pelo insumo: sem ela o LLInt não preenche o `m_modeMetadata` que a compilação lê para escolher a forma do acesso a propriedade. E `jitPolicyScale` e `forceEagerCompilation` não emitem nada, reescrevem outras options in place antes de qualquer compilação.

Em ARM, o `JITRightShiftGenerator` pergunta se há FEAT_JSCVT e emite `fjcvtzs` ou o caminho de truncamento, e as duas formas diferem no conjunto de caminhos lentos que o bytecode deixa. No x86, `addDouble` e `convertInt32ToDouble` escolhem AVX ou SSE2 durante a emissão dos caminhos aritméticos. No Linux, `collectCPUFeatures` só habilita AVX quando o processador suporta AVX e o sistema operacional preserva o estado XMM/YMM.

Nem todo `CodeBlock` continua coletando perfil no baseline. O critério é `capabilityLevel()`, lido no topo da compilação e válido para o CB inteiro: o DFG precisa estar ligado, o `ScriptExecutable` não pode ter sido marcado com `setNeverOptimize` (só o harness de teste escreve o flag), e o custo do bytecode precisa ficar em no máximo 100.000 (por CB; geralmente o maior é o de módulo e, mesmo assim, hono, express e elysia gastam todos menos que 17% do limite; no FTL o teto é 60.000). Daí o baseline deriva dois booleanos, que recebem o mesmo valor neste pin mas controlam coisas diferentes: `m_shouldEmitProfiling` liga ou desliga em bloco a escrita de perfil de valor, de array e de argumento, e a do perfil aritmético em `div`, `to_number` e `to_numeric`; `m_canBeOptimized` faz o mesmo com os contadores de tier-up de `op_enter` e `loop_hint`. Nos Math ICs (`add`, `sub`, `mul` e `negate`) o perfil aritmético continua sendo escrito com o booleano desligado: o caminho rápido emitido e o snippet que o IC regenera depois marcam bits de resultado, e a operação lenta registra os tipos dos operandos. O booleano só decide se ela registra também o resultado.

Tudo que ela lê, e o que cada coisa decide no código que sai:

| o que ela lê | de onde | o que decide no código emitido |
|---|---|---|
| `m_instructions` | UCB | o fluxo que a passada principal percorre |
| `m_constantRegisters` | UCB | `LinkTimeConstant`, `SymbolTable` e `JSTemplateObjectDescriptor` saem por load do buffer do CB (`isConstantOwnedByUnlinkedCodeBlock`); as demais entram como imediatos, com NaN normalizado para `jsNaN()`; no x86_64 esse imediato pode sair cegado. Uma constante que é célula passa por uma regra mais estrita que a da linha do switch de string: o `shouldBlind(Imm64)` peneira antes os bits dela como double, então ela precisa de dois sorteios de 1 em 64 (cerca de 1 em 4096), e o valor rotacionado cai no registrador de destino pelo `moveValue` ou no scratch de cegamento pelo `storeValue` do `op_mov`. Um int32 pula essa peneira e segue a regra do `Imm32`, um sorteio só de 1 em 64 a partir de 2^24 ou abaixo de -256, a mesma chance do switch de string |
| `m_identifiers` | UCB | o nome que vai para o molde de IC |
| tabelas de salto numérico e de caractere | UCB | mínimo, offsets e se é lista: busca binária ou tabela indireta |
| `UnlinkedStringJumpTable` | UCB | com até `maximumInlineStringSwitchCaseCount` casos (64 por padrão), emite busca binária com os ponteiros dos átomos como imediatos, que no x86_64 saem cegados por rotação quando um sorteio de 1 em 64, feito a cada emissão, acerta; acima do limite, chama o helper |
| perfis aritméticos | UCB | se sai caminho rápido de aritmética ou salto para o lento |
| forma do quadro | UCB | tamanho do call frame, se há verificador de aridade, se há perfil de argumento |
| `functionDecl`, `functionExpr` | UCB | os bits builtin, arrow e strict: qual operação C++ o `new_func` chama |
| `MetadataTable` | desenho no UCB, instância no CB | o deslocamento de cada metadata por opcode |
| metadata de IC | CB | `m_modeMetadata`, `m_resolveType`, `m_localScopeDepth`, `m_getPutInfo`: qual variante sai |
| `capabilityLevel()` | CB | se o código emitido carrega escrita de perfil e contador de tier-up |
| célula de constante e a `Structure` dela | heap | de quem a constante é, por `inherits<>` |
| `RegExp`, `UnlinkedFunctionExecutable` | heap | a célula do `new_regexp`, os bits do executável |
| `StringImpl` | fora do heap | marca de átomo e identidade, para os caminhos rápidos de `stricteq` e `switch_string` |
| `VM` | por VM | limite da stack, bits de trap, limiar de barreira, `topEntryFrame`, os trampolins do caminho lento |
| `Options` | por processo | as citadas acima: acrescentam instrução, ou decidem se famílias inteiras chegam a ser emitidas |
| features da CPU | por processo | no ARM64, FEAT_JSCVT escolhe a forma do `rshift` e seus caminhos lentos; no x86, AVX ou SSE2 nos caminhos aritméticos |

# Emissão

## Etapas

1. Ela começa emitindo a entrada da função. Como o `CodeBlock` sempre está no call frame, num slot fixo, o código emitido o alcança com um load de `[framePointer + deslocamento]`. Desse `CodeBlock` saem, num par, os dois registradores-base um aponta para o `BaselineJITData` daquela instância, onde ficam os ics e o pool de constantes, o outro para a `MetadataTable`, onde ficam o perfil de valor, o de array e a metadata de cache. Quase todo acesso a esses dois é registrador mais deslocamento constante; as exceções são o perfil de argumento, que recarrega o `CodeBlock`, e o perfil aritmético, o `profile_type`, o `profile_control_flow` e o Math IC, que alcançam o alvo por endereço absoluto. Ainda aqui saem a checagem de estouro de stack, o salvamento dos registradores que a função precisa preservar e, se ela coleta perfil, uma cópia de cada argumento para o balde dele.

2. Depois ela anda o bytecode em ordem, chamando para cada opcode a função C++ que o emite (`emit_op_add`, `emit_op_get_by_id`) e guardando em `m_labels` a posição em que cada instrução começou. Aqui nascem duas dívidas. Um `if`/`while`/`for`/ternário/qualquer desvio do JS vira um salto em asm que pula o ramo não tomado, mas, quando ela chega no salto, o bytecode de destino ainda não foi emitido (menos os que saltam para trás, mas ficam pendentes mesmo assim e são resolvidos em batch com os outros); por isso ele fica pendente em `m_jmpTable`, guardando o offset do bytecode atual mais o deslocamento que a instrução carrega. A outra dívida é saltos de testes que podem falhar, o `jo` de uma soma que estoura ou o `jb` de um teste de tipo: em teoria o caminho lento poderia sair logo abaixo da própria operação, mas eles são deixados depois dos caminhos rápidos, para ficarem mais perto uns dos outros e gastarem menos cache (além de economizar um `jmp` em quase todo opcode, que precisaria pular o lento). Esse salto sai sem destino, anotado em `m_slowCases`. Nos `switch_string` com despacho inline, os destinos dos casos também ficam em `m_jmpTable`, mas a chamada lenta sai na própria passada, logo após os testes de string atomizada.

3. Terminada a passada, toda posição já é conhecida, e uma varredura resolve cada pendência de `m_jmpTable` contra a posição do bytecode de destino. Em x86, escreve o deslocamento dentro da própria instrução, ali no buffer; em ARM64 só registra a ligação, quem escreve é a cópia para a memória executável.

4. Por último ela varre `m_slowCases` e, para cada bytecode com saltos ali, emite o caminho lento dele. Um bytecode pode ter deixado vários — um `add` de dois operandos variáveis com perfil int32 deixa três, dois testes de tipo e o estouro —, e duas asserções (release) por bytecode conferem que o emissor lento pegou exatamente os seus. No fim há mais uma asserção por família, conferindo que todo molde emitido foi consumido pelo caminho lento dele, mas são oito e as famílias são doze: `get_by_val`, `in_by_val`, `del_by_id` e `del_by_val` ficam de fora.

Um bit atravessa as quatro etapas: `m_isShareable`, que nasce verdadeiro e é derrubado por `emit_op_profile_type` e `emit_op_profile_control_flow`, porque os dois gravam no machine code um ponteiro vindo da metadata daquele `CodeBlock`. Os dois opcodes só existem com o type profiler ou o de fluxo de controle ligado, duas tools do Web Inspector desativadas por padrão.

Aqui um IC sai como molde em `m_unlinkedPropertyInlineCaches`: a compilação preenche o tipo de acesso, nome da propriedade e a forma escolhida, deixando `doneLocation` para o link preencher. A forma vem do LLInt, a compilação le `m_modeMetadata` daquele opcode e grava se o acesso é próprio ou de protótipo. O IC nasce depois, na instalação, um por CB a partir desse cara; com o índice do molde - `jitDataRegister` na instrução.

O `FunctionExecutable` que um `new_func` instancia é fabricado pelo CB no `finishCreation`. Os oito opcodes de criação de função — `new_func`, `new_generator_func`, `new_async_func`, `new_async_generator_func` e as quatro variantes `_exp` — chamam `addToConstantPool` com o índice que o bytecode traz, e o que fica guardado é um par: `FunctionDecl` ou `FunctionExpr`, dizendo em qual dos dois vetores do CB procurar, mais o índice dentro dele. A posição devolvida vira deslocamento na instrução de load, dessa vez somando sobre o mesmo `jitDataRegister`, porque o pool fica do outro lado da alocação.

## Tabela

O que o objeto `JIT` acumula enquanto emite, e até onde cada coisa chega:

| estrutura | qual etapa escreve | até quando vive |
|---|---|---|
| `m_labels` | 2 | morre com o objeto `JIT`; o mapa de bytecode do artefato é derivado dela no link |
| `m_jmpTable` | 2 | esvaziada na 3 |
| `m_slowCases` | 2 | percorrida na 4; morre com o `JIT` |
| os doze vetores de gerador de cache | 2 e 4 | morrem com o `JIT`, depois de o link preencher `doneLocation` em cada molde |
| `m_unlinkedPropertyInlineCaches` | 2 | sobrevive: movido para o artefato no link, com a ordem preservada |
| `m_unlinkedCalls` | 2 | sobrevive: movido no link e ordenado por `bytecodeIndex` |
| `m_callCompilationInfo` | 2 | lida no link, quando o `doneLocation` migra; morre com o `JIT` |
| `m_switches` | 2 | lida no link, ao preencher as tabelas; morre com o `JIT` |
| `m_switchJumpTables`, `m_stringSwitchJumpTables` | alocadas antes da 1, preenchidas na 2 e no link | sobrevivem |
| `m_constantPool` | 2 | sobrevive |
| `m_farCalls` | 2 e 4 | lida no link, ao ligar as chamadas a função do motor; morre com o `JIT` |
| os Math ICs | criados na 2, estado de caminho lento na 4, pontos finalizados no link | sobrevivem, adotados pelo artefato |
| `m_pcToCodeOriginMapBuilder` | 1, 2 e 4 | lido no link para construir o mapa, e só se a VM pedir o mapeamento |
| `m_isShareable` | 2 | copiado para o artefato no link |

# Link

1. O construtor do `LinkBuffer` aloca a memória executável e copia o buffer para lá, transformando a posição em endereço. Em ARM64 toda instrução tem 4 bytes, e um desvio condicional só alcança ±1 MB no `b.cond` e no `cbz` e ±32 KB no `tbz`, então o assembler reserva duas instruções para eles (a condição invertida por cima de um `b`, que vai até ±128 MB). Na cópia ele deixa uma só onde a distância couber, limite que o JSC reduz a ±8 KB no `tbz`, empurrando para trás tudo que vem depois, e anotando por cima do buffer de emissão quanto cada posição andou. Em x86 nada disso acontece: instrução de tamanho variável, e todo salto já com deslocamento de 32 bits.

2. Com o código no lugar, o link preenche o que dependia de endereço, em ordem: as tabelas de salto do `switch`, as chamadas às funções C++ do motor, e então os moldes de cache. São doze chamadas, uma por coleção de geradores, e cada uma preenche `doneLocation` nos moldes. O laço seguinte faz o mesmo nos registros de chamada.

3. Ela percorre as posições que a passada anotou por bytecode e guarda, para cada uma, o endereço onde ela caiu — é o que sobrevive daquele vetor. Quem consulta é a instalação, para religar os handlers de exceção, e o OSR, para entrar no meio da função, os dois por busca, já que os vetores por dentro são privados.

4. Por último ela escreve o que ficou para depois — em x86 os saltos para os trampolins do motor, que em ARM64 já saíram na cópia — e esvazia o cache de instrução (se não, o processador ARM pode buscar bytes antigos).

O que sobreviveu é movido para o `BaselineJITCode`: os ICs com a ordem intacta, porque a posição de cada um está gravada na instrução que o alcança, e os registros de chamada ordenados por índice de bytecode, procurados assim pelo OSR exit do DFG e do FTL. Aí o `LinkBuffer` e o objeto `JIT` morrem, levando junto toda a tradução de posição em endereço.

# Artefato

## Partilha

O machine code guarda o teste da especulação, e o valor conferido vem do cache vivo de quem instalou — é o que faz o mesmo código servir a mais de um `CodeBlock` do mesmo corpo. A aritmética chega ao mesmo lugar por outro caminho: o perfil dela mora no UCB, então todo `CodeBlock` daquele corpo escreve e lê o mesmo.

O que cada dado por-`CodeBlock` vira no código, e o que acontece com ele noutro `CodeBlock`:

| dado do `CodeBlock` que entrou na compilação | o que sai no código | noutro `CodeBlock` |
|---|---|---|
| `m_modeMetadata`, no `get_by_id` e nos passos de iterador | acesso próprio ou pelo protótipo, com guarda contra a estrutura do cache vivo | correto; divergir custa um caminho lento |
| `m_enumeratorMetadata`, no `enumerator_next` | caminho rápido de estrutura própria, com guarda nos flags do enumerator | correto; divergir custa um caminho lento |
| `ResolveType` global, no `resolve_scope` | `branch32` contra o tipo lido na compilação, antes de usar o membro | correto; divergir custa um caminho lento |
| `ResolveType` `ModuleVar` | um `loadPtr` num deslocamento fixo da metadata, sem guarda | o que está naquele deslocamento depende do tipo que a linkagem resolveu: num irmão que linkou `ClosureVar`, é um `SymbolTable*` |
| profundidade de closure abaixo de oito | a cadeia desenrolada, um `loadPtr` por elo | a profundidade daquele `CodeBlock` virou contagem de instrução |
| `couldBeTainted()` | um `store8` emitido pelo `op_enter`, o bytecode 0 da passada principal | nada reconfere |

A divisão entre as linhas com guarda e as sem segue o que pode mudar entre linkagens. As guardadas são as globais: um `let` global passa a sombrear uma propriedade, então o tipo daquele acesso muda, e o código emitido reconfere antes de usar o membro. As sem guarda são as léxicas, cuja forma vem do aninhamento do texto e é a mesma em qualquer linkagem daquele corpo. No `ModuleVar` o que sai é um `loadPtr` num deslocamento fixo: os cinco membros da união — ambiente léxico, symbol table, escopo constante, ambiente léxico global e objeto global — moram no mesmo lugar. O comentário no local declara que perfilar esses tipos leva todo código linkado ao mesmo tipo. A closure abaixo de oito não lê união nenhuma, porque a profundidade virou quantidade de instrução, e de oito para cima ela volta a sair da metadata, num laço que lê a profundidade viva. O que a mantém a mesma entre linkagens é a estrutura do texto.

## Campos

Das classes base do `BaselineJITCode` vêm o código em si, o ponto de entrada com verificação de aridade e quatro `Bag` de Math IC, essas herdadas do `MathICHolder`. O `MathICHolder` não expõe acessor para nenhuma delas: depois que a compilação acaba, nada liga um Math IC de volta ao bytecode que o gerou, e o único jeito de alcançar um é o ponteiro que a compilação gravou dentro do código.

O `m_livenessRate` e o `m_fullnessRate` são rascunho da decisão de subir para o DFG: ela mede a cobertura dos perfis e, quando adia, grava a medida aqui para comparar na tentativa seguinte. Num artefato novo os dois estão em zero, e num partilhado o segundo `CodeBlock` lê a medida do vizinho — estado de um `CodeBlock` guardado num objeto que pode ser de vários. Os Math ICs também mudam depois de o artefato existir, e ali a partilha acompanha o perfil aritmético, que já mora no UCB: existem só para `add`, `mul`, `sub` e `negate`, e são reescritos no primeiro miss, que é a primeira execução quando o perfil estava vazio na compilação.

Os onze campos que a classe declara com `USE(BUN_JSC_ADDITIONS)`:

| campo | o que é | quem preenche no artefato |
|---|---|---|
| `m_unlinkedCalls` | um registro por bytecode de chamada, fora o `call_direct_eval`, com o `bytecodeIndex` e o ponto onde a chamada volta ao fluxo; existe para o DFG e o FTL acharem o endereço de retorno de um quadro inlinado ao sair por OSR | `JIT::link`, movendo do vetor segmentado e ordenando por `bytecodeIndex` |
| `m_unlinkedPropertyInlineCaches` | os moldes de IC, com `doneLocation` preenchido | `JIT::link`, movendo com a ordem preservada, depois de `finalizeICs` preencher `doneLocation` |
| `m_switchJumpTables` | os destinos de um `switch` numérico ou de caractere: na forma densa, um por valor do intervalo, mais o default | `JIT::link`, movendo do vetor do compilador |
| `m_stringSwitchJumpTables` | o mesmo para `switch` de string | idem |
| `m_jitCodeMap` | índice de bytecode → endereço, consultável só por busca | `JIT::link`, acumulando os pares e montando a tabela no fim, junto com a migração |
| `m_constantPool` | tipo mais índice de executável de função; o ponteiro só é resolvido na instalação, no pool vivo do `BaselineJITData` | `JIT::link`, movendo do vetor do compilador |
| `m_pcToCodeOriginMap` | endereço → origem de bytecode, comprimido por delta | `JIT::link`, e só quando a VM pede o mapeamento |
| `m_ownerWentAwayAt` | o instante do último GC, registrado na destruição mais recente de um `CodeBlock` que usava o artefato; o GC a consulta para decidir se larga a cópia guardada no UCB | `CodeBlock::~CodeBlock`, com `vm.heap.lastGCBoundaryTime()` |
| `m_livenessRate` | fração dos perfis de valor, excluindo argumentos, com predição diferente de `SpecNone` após o dreno | ninguém na compilação; escrito depois, na decisão de subir para o DFG |
| `m_fullnessRate` | fração dos perfis de valor, incluindo argumentos, com predição diferente de `SpecNone` após o dreno | idem |
| `m_isShareable` | se este machine code pode servir a outro `CodeBlock` do mesmo corpo | `JIT::link`, copiando do compilador |
