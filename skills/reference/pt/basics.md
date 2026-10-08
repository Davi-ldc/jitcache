# Abstract

O JSC compila os tiers do JIT por `CodeBlock`s, delimitados pelo corpo de uma função + module level (que é como se fosse uma função main). No mesmo `.js` uma função pode estar em FTL e outra em baseline. A compilação normalmente é assíncrona (com `useConcurrentJIT` desligado ela roda na própria thread que pediu de forma síncrona), cada `CodeBlock` compilado por uma thread do jit pool enquanto a execução segue no interpretador/tier anterior. No Linux, o pool usa por padrão `max(min(núcleos, 3), 2)` threads; em Darwin/ARM64, o mínimo é 1 e o máximo é `min(4, núcleos)` (`Options::initialize`). Para instalar, a execução de JS para e a instalação roda na thread que a estava executando.

Internamente, são 2 estruturas principais. O `UnlinkedCodeBlock`[^unlinked] carrega tudo que não depende do globalObject (o realm — o objeto que guarda os globais e os intrínsecos daquele contexto): o bytecode, as constantes literais, os nomes, o desenho da tabela de metadados e o que se aprendeu sobre tipos em execução. O `CodeBlock`[^codeblock] aponta para ele e carrega tudo associado a um realm: onde cada nome global foi parar, os formatos de objeto observados e os destinos de chamada. 

O `BaselineJITCode` carrega o código de máquina e as tabelas que localizam um ponto dentro dele, um por compilação. A instalação o pendura no slot do `UnlinkedCodeBlock`, de onde ele serve os outros `CodeBlock`s daquele corpo (se o slot estiver vazio, `useBaselineJITCodeSharing` estiver ligada e o corpo não tiver `op_profile_type` nem `op_profile_control_flow` [dois opcodes random do Web Inspector]). Além disso, cada `CodeBlock` segura o `BaselineJITData` que a instalação montou para ele, e a tabela de metadados. O código emitido chega nas duas por `jitDataRegister` e `metadataTableRegister`, que o prólogo carrega do `CodeBlock` do quadro.

LLInt e Baseline coletam dois tipos de feedback em bytecodes específicos: **profiles**, que registram o que passou para um tier futuro especular (os de modos de iteração e de alocação de array também são lidos em runtime pelo caminho lento que os escreveu, e o aritmético pelo Math IC ao regenerar o caminho rápido), e **inline caches**, que guardam a estrutura de um objeto e a posição da propriedade ou o destino de uma chamada, trocando uma busca por if(teste)->acesso inline, otimizando o tier atual e servindo de insumo para os próximos. Só LLInt e Baseline coletam profiles (são as mesmas estruturas); DFG e FTL acumulam inline caches próprios e OSR exits, e o exit escreve nesses profiles, nunca num cache.

# Geração de UCBs

O `CodeBlock` é criado por `ScriptExecutable::newCodeBlockFor` a partir de um `UnlinkedCodeBlock`, que pode já existir, vir do cache de bytecode ou precisar ser gerado do fonte. Durante a geração, cada função aninhada vira um `UnlinkedFunctionExecutable` guardado no unlinked — nome, faixa de fonte e modo, sem AST e sem bytecode. No caminho lazy normal, quando o LLInt começa a rodar existe um par só, o do topo; o resto nasce na primeira chamada daquela função naquele modo.

Gerado o bytecode, o `CodeBlock` nasce em duas etapas. O unlinked guarda a descrição do layout dos metadados, sem reservar o buffer de uma tabela completa. Na linkagem, `UnlinkedMetadataTable::link()` aloca um buffer zerado e preenche os offsets. Destruir a tabela libera esse buffer, deixando no unlinked só o necessário pra reconstruir o layout. O `finishCreation` monta o resto: as constantes viram valores daquele realm, com a `SymbolTable` clonada; cada função aninhada ganha um `FunctionExecutable` próprio (numa declaração de módulo ele reaproveita o executável criado na instanciação); a tabela de handlers de exceção é copiada com cada `nativeCode` apontando para o despachante `op_catch` do LLInt; e uma passada pelo bytecode inicializa o `CallLinkInfo` de cada chamada e os perfis. O template object, o array de strings de um template literal, fica para o final porque pode lançar, e uma exceção no meio da linkagem quebraria o CB.

O “bytecodecaching” salva o bytecode e os dados unlinked necessários pra reconstruí-lo, incluindo os executáveis das funções aninhadas, mas não o aquecimento: os vetores de profiles são recriados sem observações e o contador de tier-up do LLInt é reinicializado.

# Baseline

No baseline, cada bytecode que deixa um ou mais saltos em `m_slowCases` forma um diamond: caminho rápido, caminho lento e ponto de encontro. Opcodes sem caso lento, como `mov`, `jmp`, `nop` e `ret`, ficam só na passada principal. Fisicamente os dois lados ficam longe um do outro: o JSC emite primeiro todos os caminhos rápidos em ordem de bytecode e depois todos os caminhos lentos juntos num bloco:

```
entrada normal  ->  prólogo
                    caminho rápido do bytecode 0, 1, 2, …   (junções aqui)
                    todos os caminhos lentos, em bloco      (cada um volta para a sua junção)
entrada com     ->  verificador de argumentos               (corrige a aridade e salta para a entrada normal)
aridade
                    epílogo de stack overflow               (sempre emitido; segundo prólogo e saída por thunk)
```

repara que a entrada com verificação de argumentos fica num endereço **maior** que a entrada normal, e que ela só existe quando o `CodeBlock` é de função e tem ao menos um parâmetro declarado — sem isso as duas entradas são o mesmo endereço.

## Exemplo

O pipeline básico do JSC é parser → AST → BytecodeGenerator → bytecode (tipo um IR portátil). Para `function f(a, b) { return a + b; }` ele vai ser algo tipo:

```
[ 0] enter
[ 1] add  dst:loc5, lhs:arg1, rhs:arg2    ; some arg1 com arg2 e ponha o resultado em loc5
[ 7] ret  value:loc5
```

if necessary check [bytecodeoptcodes.md](./bytecodeoptcodes.md) for all opcodes

Repara que, em teoria, nada impede de compilar um baseline logo no call 0 mas teria um startup time gigantesco e gastaria bem mais memória. O LLInt tem fast paths hardcoded; o baseline emite a mesma forma para quase todo opcode, e consulta o que foi observado só onde a forma muda.

Prum `add`, llint (já vem carregado) faz:

```
mov 0x0(%rbp,%rdx,8), %rax
cmp %r14, %rax / jb <double>   ; int32?
cmp %r14, %rsi / jb <double>   ; int32?
add %esi, %eax                 ; ← fast path hardcoded
jo  <lento>                    ; 
or  %r14, %rax                 ; devolve a tag de int32 (pra somar o i32 que fica num JSValue nan-boxed 
                               ;                         precisa tirar a tag da frente)
movq %rax, 0x0(%rbp,<dst>,8)   ; escreve
...                            ; anota no perfil aritmético que passou int32
loadb / jmp opcodeMap[op]      ; próximo bytecode
```

Já o baseline, dependendo do perfil, gera:
```
                         (mesmo do llint)         (int+int, int+double, double+int e double+double, tudo inline)
── perfil VAZIO          ── perfil INT32          ── perfil DOUBLE                   ── perfil STRING
jmp <lento>              cmp %r14, %rsi           cmp %r14, %rsi                     movq 0x8(%r13), %rdi
                         jb  <lento>              jb  <double>                       movl $0x1, 0x24(%rbp)
                         cmp %r14, %rdx           cmp %r14, %rdx                     mov $<operationValueAdd>, %r11
                         jb  <lento>              jb  <double>                       call %r11
                         mov %esi, %eax           ... (o mesmo par int32 daqui)      test %rdx, %rdx
                         add %edx, %eax           <double>: vcvtsi2sd %edx, %xmm1    jnz <exceção>
                         jo  <lento>              vaddsd %xmm1, %xmm0, %xmm0         movq %rax, -0x30(%rbp)
                         or  %r14, %rax           vmovq %xmm0, %rax
                         movq %rax, -0x30(%rbp)   ...
3 instruções             11                       32                                 9
```

PS: O `add` usa um cache aritmético, que consulta o perfil e pode se reescrever ao cair no caminho lento repatcheável (`JITMathIC::generateOutOfLine`). Com perfil vazio, esse caminho é tomado na primeira execução; acertar um caminho rápido já emitido não reescreve nada.

A jogada é que, em vez de ler bytecode e fazer `llint_op_[índice do byte]`, a sequência fica inline, principal motivo de o baseline ser ~2x mais rápido que o LLInt.[^porque]

O DFG vai muito além disso, para `for (let i = 0; i < N; i++) sum += i` o Baseline paga:

```
loop:                      ; (a aresta de volta cai no check_traps, 3 instruções antes daqui)
  movq -0x30(%rbp), %rsi   ; sum, boxed
  movq -0x38(%rbp), %rdx   ; i, boxed
  cmp  %r14, %rsi
  jb   <lento>             ; sum é int32?
  cmp  %r14, %rdx
  jb   <lento>             ; i é int32?
  mov  %esi, %eax
  add  %edx, %eax
  jo   <lento>
  or   %r14, %rax          ; re-boxa
  movq %rax, -0x30(%rbp)
  …                        ; o i++ paga os mesmos testes, e o i < N de novo
```

E o DFG, tendo especulado que os dois são int32:

```
loop:
  movl -0x30(%rbp), %eax   ; sum, int32 cru no mesmo slot
  movl -0x38(%rbp), %esi   ; i
  add  %esi, %eax
  jo   <osr exit>
  movl %eax, -0x30(%rbp)
  inc  %esi
  jo   <osr exit>
  movl %esi, -0x38(%rbp)
  movl 0x30(%rbp), %eax    ; N
  cmp  %eax, %esi
  jl   loop
```

OSR, On-Stack Replacement, é trocar o código de um call frame que já está na stack, sem esperar a função retornar. Exit tira você de um código que falhou uma especulação e devolve ao baseline no ponto equivalente; entry sobe você enquanto está rodando. Cada iteração de um loop soma no contador e, atingindo o limiar, pode agendar um plano DFG pedindo uma entrada adicional no laço em que está (normalmente você só pode entrar pelo começo do CB, porque o DFG espera o estado de uma forma diferente e o constrói no começo), que lê os valores do frame baseline de um buffer auxiliar e os coloca, já sem tag, nos virtual registers que o código DFG espera no ponto da segunda entrada. Quando o plano fica pronto, a thread que compilou sinaliza zerando o contador do baseline; ao voltar ao handler, a thread JS finaliza os planos prontos, instala o código e tenta entrar por OSR. A preparação (`DFG::prepareOSREntry`) ainda pode recusar a entrada, por exemplo se os valores não baterem com o esperado, e aí a execução continua no baseline.

Essa segunda entrada fica numa tabela do código DFG, indexada por índice de bytecode, e não é deletada depois do primeiro uso: qualquer invocação que ainda esteja no baseline naquele ponto pode tentar entrar por ela. Depois da instalação, as chamadas novas entram pelo começo do código instalado.

# Relação UCBs e CBs

O que depende do realm fica no `CodeBlock`: a `SymbolTable` clonada, o template object, os `StructureID`s das formas observadas e a resolução de cada variável livre, que `JSScope::abstractResolve` calcula na linkagem e grava na metadata. A reserva de formas é uma só por processo, em `g_jscConfig`, compartilhada por todas as VMs. Em 64 bits, o `StructureID` guarda os 32 bits baixos do endereço; `decode()` limpa o bit 0 (`nuke`) e soma `structureIDBase`, que pode diferir do começo da reserva quando ela tem menos de 4 GiB. PS: fica claro depois de ler profiles e inlinecaches .md

Toda `JSC::VM` tem um `CodeCache` com entradas `Strong`, raízes de GC, que guarda UCB num hashmap. A chave combina o hash da fonte com sete flags — tipo de código, o bit de strict, o `JSParserScriptMode` (se é módulo ou não), contexto derivado, contexto de eval, contexto de arrow function e modo de geração de código —, já que o mesmo texto sob flags diferentes gera bytecode diferente. A comparação também confere o comprimento, as flags, o nome e a posição de fim dos parâmetros, que separam entradas de `new Function`, e o host da origem com `sourceOrigin().url().host()`. Sob `USE(BUN_JSC_ADDITIONS)`, `SourceCodeKey::operator==` termina nesses testes, sem comparar o texto da fonte. Entram nele quatro fontes: o topo de um programa, o topo de um módulo e um eval indireto, cada um como UCB, e o `new Function`, cuja entrada é um UFE com o UCB pendurado. Com `useCodeCache` ligada, um UCB de programa ou módulo decodificado do cache do provider também é inserido no mapa (`findCacheAndUpdateAge`). As funções aninhadas ficam nos UCBs.

Dois CBs podem dividir um UCB quando batem na mesma chave. Programa e eval indireto funcionam igual: cada avaliação fabrica um executável novo e busca o UCB no `CodeCache`. O `new Function` pede mais: o global object guarda o último executável elegível num slot fraco de uma posição só. Enquanto ele estiver vivo e os critérios de fonte e modo coincidirem, chamadas seguidas reaproveitam o executável e seu CB; outro texto no meio é uma maneira de errar o slot, mas o GC e uma origem diferente também podem fazê-lo falhar (`tryGetCachedFunctionExecutableForFunctionConstructor`). Também acontece entre realms quando os executáveis acabam apontando pro mesmo UCB.

O tier é local — um CB pode nem ter sido criado enquanto outro já está no FTL — e a escada não é obrigatória: um `CodeBlock` criado depois de o artefato ficar pendurado no unlinked já nasce em baseline, sem nunca ter interpretado.

Corpos idênticos em escopos diferentes, `function f(){}; function g(){ function f(){} }`, e a mesma função usada como `f()` e `new f()` geram `UnlinkedCodeBlock`s diferentes. No primeiro, cada função tem seu próprio UFE e seus próprios slots de bytecode; `unlinkedCodeBlockFor` consulta esses slots, sem procurar outro corpo de texto idêntico no `CodeCache`. Já no segundo o bytecode é diferente: `generateUnlinkedFunctionCodeBlock` recebe o `CodeSpecializationKind` e o repassa ao `ExecutableInfo` como o bit de construtor, que o `BytecodeGenerator` consulta em uma dezena de pontos — desliga otimização de chamada em cauda, muda o prólogo, e só então emite inicializador de campo de classe. 

# Memória executável

O machine code mora numa reserva criada uma vez por processo, 512 MB no ARM e 1 GB no x86. Ela é reservada inteira de uma vez porque salto é um endereço embutido na instrução: se um segundo mmap caísse GBs à frente, ele precisaria virar um `movz`/`movk` mais um salto por registrador, o que empurraria tudo embaixo e cascatearia em cada deslocamento já gravado. No x86 um `call` relativo alcança ±2 GB e cobre o pool inteiro; no ARM um `bl` vai só até ±128 MB, por isso das ilhas.

Para alcançar mais longe que isso dentro do próprio pool, o ARM planta no meio do caminho uma ilha, um salto de 4 bytes que não faz mais nada. O pool é fatiado em regiões de 112 MB, os últimos 16 MB de cada uma sendo banda de ilha; no ARM, quatro delas cabem na reserva e a quinta entra truncada, com 64 MB de código e banda nenhuma. Um salto que cruza regiões vira corrente, código, ilha, ilha, código, e cada elo cabe no alcance. Se a banda de uma região esgotar e for preciso mais uma, o processo morre.

Quem pede memória executável passa junto um `JITCompilationEffort`, `CanFail` ou `MustSucceed`. Podendo falhar, `ExecutableAllocator::allocate` compara o que já está alocado mais o pedido contra `(pool − ilhas) × 0,75`, e devolve nulo sem tocar no alocador se estourar. Não podendo, pula essa conta, e se não funcionar chama `CRASH()`.

Só o que é essencial para rodar não aceita falhar: os thunks, o nascimento de um `CodeBlock` baseline com o LLInt desligado, e o caminho lento lazy do FTL. As compilações que têm para onde voltar, do baseline para frente, mais o stub de um cache inline e a regeneração de um Math IC, falham em silêncio quando tentam copiar o código pronto para a memória executável: o cache desiste e fica no caminho lento, o Math IC reescreve a chamada para apontar para um caminho lento que não pode ser repatcheado e desiste de regenerar para sempre, o tier-up não acontece.

# Tier-up

Cada subida funciona com pontos (500 para sair do LLInt, 1000 para o DFG, 64000 para o FTL). Uma passagem completa de função vale 15 e cada volta de loop (for/for-in/for-of/while/do-while) vale 1. No LLInt os 15 são divididos entre prólogo e epílogo, 5 e 10; no baseline vão inteiros na entrada, onde ele já materializa os registradores base a partir do `CodeBlock`; e no DFG vão no retorno, porque um OSR exit no começo ou no meio contaria uma passagem que não terminou.

Quando ele chega a zero ou mais, cai no handler de tier-up, que compara o total de execuções já contadas contra o limiar corrigido: o base multiplicado pela razão `disponível / (disponível − alocado − estimativa)`, onde a estimativa de custo é `(média + desvio padrão) × custo` e a média sai dos bytes de pool por palavra de bytecode que a `VM` acumula a cada compilação baseline, e subtraído por `min(limiar inicial, teto de fatia) / 2`. O disponível aqui é a reserva menos as bandas de ilha e menos 25% de margem. A razão só vale enquanto `alocado + estimativa < disponível`; alcançando ou ultrapassando esse espaço, `ExecutableAllocator::memoryPressureMultiplier` devolve 1,0. 

Se o valor final for maior que o teto de 1000 no baseline, 30000 nos tiers de cima, ele capa o valor, e quando atingir o teto recalcula o limiar contra a ocupação daquele instante e arma a fatia seguinte. No baseline, quando o custo de bytecode do `CodeBlock` chega a dez mil (o custo é somado instrução a instrução, cada uma valendo o tamanho do seu opcode mais um), o teto é multiplicado pela raiz do fator de tamanho, `0,826 + 0,0615·√(custo + 1,024)`, porque cada parada dessas também drena perfil e drenar perfil de função gigante toda hora sai caro.

De baseline→DFG para cima, o limiar armado é multiplicado pelo mesmo fator de tamanho, qualquer que seja o custo, e por dois elevado ao número de reotimizações já tentadas (capado a 21).

Em baseline→DFG, especificamente, antes de aprovar uma compilação o handler drena os perfis de valor e mede duas coberturas: a fração com predição diferente de `SpecNone` entre os perfis que não são de argumento, que precisa dar 0,75, e entre todos os perfis, incluindo argumentos, que precisa dar 0,35. São predições acumuladas, não a ocupação atual dos baldes (`shouldOptimizeNowFromBaseline`); a ideia é evitar especular em cima de perfil vazio e tomar OSR exit na primeira execução. No quick tier-up do Mac, as duas exigências são multiplicadas por 0,85. Uma categoria sem perfis conta como 1,0. Corpos com custo de bytecode de pelo menos 5000 também passam quando as duas taxas são não zero e ficaram iguais às da tentativa anterior, porque esperar mais pode não ensinar nada. Fora desses casos, ele adia de verdade: rearma o limiar cheio e devolve não, então o corpo volta a contar antes de ser reavaliado. Se chegar a cinco adiamentos, deixa a exigência de cobertura pra lá e permite compilar. Esses números são os padrões das `Options`.

São três campos: `m_counter` é o contador de pontos atual, `m_totalCount` soma os pontos que ele já ganhou mais o limiar pendente, de modo que as execuções contadas até aqui são sempre `m_totalCount + m_counter`, e `m_activeThreshold` é o limiar base que a correção multiplica a cada parada. As três subidas usam o mesmo contador, mas em lugares diferentes: LLInt→baseline mora no `UnlinkedCodeBlock`, (todo CB daquele corpo soma no mesmo); baseline→DFG mora no `BaselineJITData`, um por `CodeBlock`; o de DFG→FTL no `DFG::JITData`, um por CB do DFG, mas com o limiar calculado a partir do CB baseline.

No LLInt→baseline o limiar armado é o base × 1 se o motor ainda não sabe nada daquele UCB, × 4 se algum `CodeBlock` dele morreu antes de qualquer um entrar no DFG por um OSR entry que deu certo (o destrutor não confere se aquele CB chegou ao DFG), e ÷ 2 depois que um deles entra assim, o que prevalece dali em diante. Não basta ter compilado nem chamar o código do DFG pela entrada normal da função; vale o OSR entry de um laço ou o da primeira instrução, que o `op_enter` dispara quando o contador cruza o limiar.

## Jettison

Jettison é descartar um código. Num tier otimizado, normalmente volta a executar o baseline. São sete causas: sair por OSR exit demais (cada saída de especulação soma no `CodeBlock` que saiu — checagem de exceção e desempilhamento não contam —, e passando de 100 ele é jettisonado, ou de 5 quando o baseline daquele corpo já voltou a pedir para subir ou alguma função inlinada na cadeia já tentou entrar por OSR num loop; os dois números são multiplicados pelo mesmo dois elevado ao número de reotimizações já tentadas), um watchpoint disparar, uma entrada por OSR que não deu certo, uma referência fraca morrer, idade, debugger e traps da VM.

Abaixo desse limiar a saída não joga nada fora, mas mexe no contador do corpo: o próprio stub de exit o reseta de volta ao limiar cheio, senão o baseline mandaria recompilar em seguida o mesmo código que acabou de falhar. Se o contador já estiver pedindo para subir, a saída chama `operationTriggerReoptimizationNow`, que reconfirma os critérios de exits; sem exits suficientes, só rearma o contador com `optimizeAfterLongWarmUp()`. E jettisonando por qualquer razão que não seja idade ou traps, os pontos de bytecode que já saíram por exit são gravados no perfil de saída do `UnlinkedCodeBlock`, que sobrevive ao `CodeBlock`: é assim que a compilação seguinte sabe onde especular menos.

As causas que são falha de especulação (exit demais, watchpoint, entrada por OSR que não deu certo) mexem no limiar futuro, incrementando o contador de reotimizações e apagando o bit de quick tier-up daquele tier no `UnlinkedCodeBlock`, que registrava que aquele corpo já tinha provado valer a subida. O do DFG acende quando o DFG é instalado e, aceso, multiplica o limiar por 0,2 fora do Mac e por 0,15 no Mac; o do FTL acende quando o FTL é instalado, mas só reduz limiar no Mac, com o mesmo 0,15 — fora dele o fator é 1 e o bit não muda nada. O do FTL volta a acender a cada instalação em qualquer plataforma; o do DFG só no Mac, e em todo o resto, uma vez apagado, não volta. Falha de compilação também apaga.

Depois que o handler confirma, o que muda entre os tiers é como cada um descobre que a compilação ficou pronta. Em LLInt→baseline ele enfileira e não rearma: o contador fica em zero, então toda chamada seguinte cai nele de novo, e uma delas encontra o código pronto. Em baseline→DFG ele enfileira e também sai sem tocar no contador, que continua cruzado e é rearmado na entrada seguinte com a thread ainda compilando. De DFG→FTL o rearme já sai no enfileiramento. Daí para frente os 2 últimos são iguais — a execução volta a contar até o limiar cheio, e, se for reatingido com a compilação ainda em curso o handler rearma de novo, bate no worklist e recebe `Compiling`. Quando o plano fica pronto, a thread que compilou sinaliza zerando o contador. Essa escrita pode correr com o rearme da thread JS, então não garante a parada imediatamente seguinte; ao voltar ao handler, a thread JS finaliza e instala os planos prontos (`setOptimizationThresholdBasedOnCompilationResult`).

O limiar é agressivo com saídas em loop porque ali o reset custa caro, ele continua girando em baseline até ele esquentar um limiar inteiro de novo. Fora de loop a saída devolve uma invocação; dentro dele, devolve o resto do laço. Com 5 em vez de 100, o motor prefere recompilar logo, com o que a saída ensinou, a pagar vinte vezes esse desperdício.

# Locks

Três tipos de thread olham para o mesmo `CodeBlock`: as que rodam JS, as de compilação e as do GC.

As threads que rodam JS são as únicas que podem alocar `JSCell`s, criar `Identifier` e mutar `Structure`. O lock principal que permite isso é o `JSLock`, um por `JSC::VM` (`VM::m_apiLock`), que também garante que só uma thread roda JS naquela VM. `JSLock::lock` o adquire; depois, `didAcquireLock` obtém acesso ao heap, se necessário, e registra a thread no coletor quando o `uid` muda.

As threads de compilação nunca alocam `JSCell`, e o construtor afirma isso com `ASSERT(!isCompilationThread())`. Mas leem e mutam o heap: `CodeBlock::capabilityLevel()` memoiza escrevendo `m_capabilityLevelState`, e `JITPlan::iterateCodeBlocksForGC` generaliza — "Compilation writes lots of values to a CodeBlock without performing an explicit barrier. So, we need to be pessimistic and assume that all our CodeBlocks must be visited during GC".

Marcar é seguir ponteiros, marcando tudo que é alcançável a partir das stacks das threads JS e das outras raízes que a `JSC::VM` e o heap visitam (`Heap::addCoreConstraints` e `gatherVMRoots`). Entre elas estão os `Strong`, handles que o C++ declara como raiz; os valores que o host protegeu com `JSValueProtect`; a exceção em propagação, guardada num campo da `VM` enquanto o `throw` sobe; as listas de argumentos em trânsito; as strings pequenas; e os `CodeBlock`s com compilação em andamento. No heap isso funciona com o JS rodando, já que os `WriteBarrier`s avisam o GC quando referências mudam; na stack, isso seria caro demais, então o stop the world precisa parar as threads para marcar — as de JS pela stack, as de compilação porque escrevem no `CodeBlock` sem `WriteBarrier`. 

Nas de compilação, o GC toma o `m_rightToRun` de cada `JITWorklistThread`, que cada uma segura enquanto trabalha. Pra parar a thread JS, `Heap::stopTheMutator` olha o `hasAccessBit` em `m_worldState`: sem acesso ao heap, pode marcar o mundo parado e conduzir a coleta; se a thread ainda detém esse acesso, acende o `mutatorHasConnBit` e agenda um timer no run loop. A thread coopera por `stopIfNecessary`, nos caminhos lentos de alocação e no timer, assumindo a condução das fases. Ao devolver a condução com `relinquishConn`, apaga esse bit ainda com heap access. O que decide essa passagem é a posse do acesso ao heap, não simplesmente estar ou não executando JS.

O DFG e o FTL cedem entre fases, então o stop the world espera por eles; o baseline abre um safepoint só e compila inteiro lá dentro, então o coletor entra quando quiser e a finalização roda com aquela compilação viva.

# Heap

O JSC tem quatro heaps `mmap`ados principais: o do bmalloc (que hoje é libpas), que segura `MetadataTable`, `BaselineJIT Data/Code`, os handlers de IC e os blocos onde as células moram; o das `Structure`s, de 4 GiB; o Gigacage, de 64 GiB, que segura typed arrays isolando o ponteiro delas contra a máscara; e o pool de memória executável, 512 MB em ARM64 e 1 GiB em x86_64.

# GC

O coletor marca o que está vivo e mata por omissão, em três atos por ciclo: marcar, que reveza janelas de mundo parado com janelas de mundo andando até uma parada não acrescentar mais nada; finalizar, com o mundo parado, que reage ao que ficou sem marca; e varrer, que é onde o destrutor corre e a memória volta. A varredura normalmente é incremental, com o mundo andando, mas também pode ser síncrona conforme a política de coleta (`Heap::shouldSweepSynchronously`). 

São dois tipos de ciclo, e os dois saem das mesmas raízes: o full apaga as marcas e percorre tudo, e o eden que não desce em nada já marcado, visitando os velhos que `WriteBarrier` anotou como mudado (pra pegar novos referenciados por velhos). "As marcas" aqui é full + edens passados des do último full; Um full seguinte descarta o acumulado todo e varre tudo. Quem escolhe é a coleta que veio antes — o coletor mantém um teto de tamanho para o heap, recalculado a cada full em proporção ao que sobreviveu a ele, e no fim de um eden, se o espaço restante cair abaixo de `minEdenToOldGenerationRatio` desse teto, a próxima sai full. O padrão é um terço de espaço restante, equivalente a passar de dois terços de ocupação (`Heap::updateAllocationLimits`).

O `CodeBlock` é um `JSCell`, mas tem regras próprias de visita e retenção. Nos `MarkedBlock`s, as células moram em blocos de 16 KiB fatiados em átomos de 16 bytes, e o cabeçalho de cada bloco carrega um bitmap com um bit por átomo, aceso quando a célula que começa ali foi marcada como viva. Nem tudo no heap é célula: há armazenamento `Auxiliary` pra buffers, e alocações grandes podem usar `PreciseAllocation` em vez desses blocos.

Na marcação, a primeira visita a um `CodeBlock` em LLInt ou baseline pelo `SlotVisitor` já drena os perfis de valor, inclusive com o mundo andando (`CodeBlock::visitChildren`). Finalizar é reagir ao resultado da marcação, e quem faz isso pelo CB é o próprio `CodeBlock`: todo bloco visitado se inscreve num conjunto — outro bitmap sobre os mesmos blocos —, e no fim o coletor cruza esse conjunto com as marcas e chama o finalizador dos que continuam marcados. Nele, em LLInt e baseline, o dreno refaz os perfis de valor e cobre também os de array e alocação de array, classificando as amostras e esvaziando os baldes mesmo quando elas continuam vivas; todo cache inline cuja estrutura morreu volta ao estado vazio; e toda chamada cujo callee morreu é desligada (`reconcileWeakReferencesAtGCEnd`). Quem ficou sem marca não tem finalizador chamado — sobre ele age o executável dono, se também estiver marcado, num finalizador próprio que roda antes, jettisonando o bloco e cortando a aresta.

# Ciclo de vida

## CB

Um CB LLInt começa com um prazo de proteção de 5 segundos; no baseline o prazo base é 15. Essa proteção vem da visita ao executável dono, não é uma raiz por si só. No perfil Bun, com `useExecutionCountForCodeBlockAging` ligada, o prazo pode ser renovado quando o contador mostra atividade, como descrito no [aging do fork](knowledge/common/bunpatches.md#aging-de-código-pr-557-codeblockshouldjettisonduetooldage). Perdendo a proteção, o CB ainda pode sobreviver se for alcançado por outra referência: numa stack, por um plano de compilação em voo ou como alternativo de um otimizado. Sem isso, pode ser coletado, e o destrutor solta a tabela de metadados e o `BaselineJITData`; não precisa ser um full se o CB ainda pertencer à geração jovem.

O `BaselineJITCode` pode continuar no unlinked, e o slot é olhado por `prepareForExecutionImpl` na criação do CB seguinte, antes de rodar um bytecode sequer, e pelo handler de tier-up do LLInt. Mas esse slot também pode ser limpo pelo `jettison` por idade ou por `Heap::releaseUnusedSharedBaselineCode` no fim de um full: com partilha e aging por contador ligados, esta última libera o código que só o UCB segura e cujo último CB dono morreu há mais de 45 s nos padrões. `deleteAllCode` também esvazia o slot. A memória executável volta ao pool quando cai a última referência, não necessariamente quando morre um CB.

Já o DFG e o FTL têm outro caminho pra sobreviver: se todas as referências fracas pras células e estruturas sobre as quais especularam estiverem vivas, `determineLiveness` pode mantê-los. No fork, esse caminho não salva um otimizado que envelheceu. Se o próprio CB for alcançado por outra referência, é ele que marca essas dependências (`stronglyVisitWeakReferences`); uma dependência que não foi marcada por outro caminho não implica, sozinha, a morte do CB.

É meio que um hunger games de CBs, mas continuar rodando já pode renovar a proteção do baseline. Se subir, o `CodeBlock` do DFG passa a apontar pra ele como alternativo, servindo de escudo pro fallback. Subir pro FTL não dá escudo pro DFG, só pro baseline, pela mesma razão. Esse escudo também cai quando o otimizado envelhece sem outra referência que o mantenha vivo (`ScriptExecutable::visitCodeBlockEdge`).

Nos stubs de IC acompanhados pelo GC, a liberação exige tanto o fim das referências quanto a confirmação de que nenhuma stack ainda os executa. Se o último dono soltar primeiro, o stub espera o GC; se o GC já o retirou do conjunto porque seus donos morreram e ele não está em execução, o último `deref` pode destruí-lo diretamente (`JITStubRoutineSet` e `GCAwareJITStubRoutine::observeZeroRefCountImpl`).

PS: um exit salta para o LLInt em vez do baseline caso o callee inlinado nunca tenha sido compilado — o DFG inlina função que só rodou no interpretador, e o fonte tem um FIXME admitindo isso.

## UCB

A árvore é: o UCB de um corpo guarda um `UnlinkedFunctionExecutable` por função aninhada, e cada um desses guarda o UCB daquele corpo depois que ele é gerado. Normalmente o UCB de uma função é segurado pelo UFE que o gerou, o UFE pelo UCB que o contém, até bater numa raiz. Os topos de programa, módulo e eval indireto podem ser mantidos por entradas do `CodeCache`; no `new Function`, a entrada é o UFE, não o UCB. Já o eval direto não passa por esse cache: seu executável fica no rare data do `CodeBlock` onde a chamada está. Perder essa aresta não garante a morte, porque um CB vivo ou uma função que escapou também pode manter o executável e a árvore alcançáveis.

A aresta UFE→UCB também pode ser fraca: com `useUnlinkedCodeBlockJettisoning` ligada (desligada por padrão) ou em mini mode, UFEs não vindos do cache podem deixar de marcar UCBs antigos cujo `didOptimize()` não seja `True`, e limpar os slots se eles ficarem sem marca (`codeBlockEdgeMayBeWeak` e `UnlinkedFunctionExecutable::visitChildrenImpl`). UFEs decodificados do cache ficam fora desse caminho. `VM::deleteAllCode` oferece também um descarte explícito dos UCBs.

O `CodeCache` se autorregula por distância de reuso: a cada busca bem sucedida no mapa compara quanto texto manuseou desde o toque anterior naquela entrada com a capacidade atual, sobe `4 × 32 × comprimento` quando a distância a supera e desce `4 × comprimento` quando ela fica abaixo da metade, respeitando um piso. Pode podar quando está acima da capacidade ou chega a 2000 entradas. Abaixo de 2000, ainda espera dez segundos desde a última poda, a menos que o conteúdo guardado tenha crescido pelo menos 16 milhões de code units nesse intervalo — crescimento líquido de `m_size`, não todo texto novo processado. Antes das remoções, atualiza o piso de capacidade a partir desse crescimento. Depois despeja `m_map.begin()` em laço até ficar dentro da capacidade e abaixo de 2000 entradas (`CodeCacheMap::prune` e `pruneSlowCase`).

Ao tirar uma entrada que seja UCB, ele chama antes `commitCachedBytecode()` que salva o bytecode em disco (se o host implementar; as virtuais de `SourceProvider` que salvariam são vazias mas tem uma API lá); se for um UFE no caso de `new Function`, pula essa parte. Tirar a entrada destrói o `Strong`, não o UCB: ele continua vivo enquanto outro caminho o alcançar. Uma função que escapou, por exemplo, mantém seu UFE e `m_topLevelExecutable` (`FunctionExecutable::visitChildrenImpl`), e um módulo registrado pode continuar preso ao module loader. A busca seguinte por aquele corpo erra o cache em memória e tenta decodificar ou gerar o bytecode; se criar um UCB novo enquanto o antigo ainda estiver vivo, os dois coexistem. Quando o UCB morre, solta o `RefPtr` de qualquer código que ainda esteja no slot de partilha, e a imagem só devolve memória executável ao pool se aquela era a última referência.

# Estruturas

[^unlinked]:

```cpp
// bytecode/UnlinkedCodeBlock.h
std::unique_ptr<JSInstructionStream>     m_instructions;      // o bytecode
FixedVector<WriteBarrier<Unknown>>       m_constantRegisters; // constantes literais
FixedVector<SourceCodeRepresentation>    m_constantsSourceCodeRepresentation;
FixedVector<Identifier>                  m_identifiers;
const Ref<UnlinkedMetadataTable>         m_metadata;          // layout
OutOfLineJumpTargets                     m_outOfLineJumpTargets; // hash; o salto que não coube no operando
FunctionExpressionVector                 m_functionDecls, m_functionExprs;
FixedVector<UnlinkedValueProfile>        m_valueProfiles;     // predição, sem baldes
FixedVector<UnlinkedArrayProfile>        m_arrayProfiles;
FixedVector<BinaryArithProfile>          m_binaryArithProfiles; // já classificados
FixedVector<UnaryArithProfile>           m_unaryArithProfiles;
DFG::ExitProfile                         m_exitProfile;
BaselineExecutionCounter                 m_llintExecuteCounter; // o contador de tier-up; todo CodeBlock do mesmo corpo compartilha um só
RefPtr<BaselineJITCode>                  m_unlinkedBaselineCode; // o slot de partilha
VirtualRegister m_thisRegister, m_scopeRegister;
unsigned m_numVars : 31;
unsigned m_numCalleeLocals : 31;
unsigned m_numParameters : 31;

std::unique_ptr<RareData> m_rareData;   // handlers de exceção e jump tables,
```

[^codeblock]:

```cpp
// bytecode/CodeBlock.h
WriteBarrier<UnlinkedCodeBlock>  m_unlinkedCode;    // aponta para o de cima
WriteBarrier<JSGlobalObject>     m_globalObject;    // o realm
WriteBarrier<ScriptExecutable>   m_ownerExecutable;
VM* const                        m_vm;
const void* const                m_instructionsRawPointer;
mutable ConcurrentJSLock         m_lock;            // protege os ICs: quem modifica um, e quem lê de outra thread
RefPtr<JSC::JITCode>             m_jitCode;
void*                            m_jitData;         // BaselineJITData ou DFG::JITData
RefPtr<MetadataTable>            m_metadata;        // a INSTÂNCIA, com os perfis. vizinho de m_jitData
                                                    // por contrato: o prólogo carrega os dois num loadPairPtr
CompressedLazyValueProfileHolder m_lazyValueProfiles;
FixedVector<ArgumentValueProfile> m_argumentValueProfiles;
Vector<WriteBarrier<Unknown>>    m_constantRegisters;
FixedVector<WriteBarrier<FunctionExecutable>> m_functionDecls;
FixedVector<WriteBarrier<FunctionExecutable>> m_functionExprs;
StructureWatchpointMap           m_llintGetByIdWatchpointMap;
SentinelLinkedList<CallLinkInfoBase, BasicRawSentinelNode<CallLinkInfoBase>> m_incomingCalls; // quem chama este CB, para religar na troca de tier
#if ENABLE(JIT)
uint8_t m_capabilityLevelState : 2;                 // memoiza o gate que liga a emissão de perfil
#endif
WriteBarrier<CodeBlock>          m_alternative;
```


# Notas

[^porque]: O LLInt termina todo handler com `loadb` do próximo opcode e `jmp opcodeMap[opcode]`, um salto indireto difícil de prever. Enquanto a instrução N executa, a CPU já busca e decodifica as seguintes; se a previsão erra, ela descarta esse trabalho e recomeça no destino. O baseline remove um dispatch desses por bytecode. O [blog do WebKit](https://webkit.org/blog/10308/speculation-in-javascriptcore/) chama essa remoção de principal motivo do ganho e resume o baseline como ~2× mais rápido que o LLInt.

PS: O jsc quatro linguagens — C++, o offlineasm do LLInt (.asm, traduzido no build), os .rb que geram bytecode e metadata a partir de BytecodeList.rb, e o assembly que os emissores produzem em runtime. cuidado com greep só em cpp especialmente pra falar do llint