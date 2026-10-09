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

## Cross-compilação aarch64

O LeakSanitizer não roda sob QEMU user mode, que recusa com EINVAL o `clone` do tracer dele, então as checagens de vazamento em aarch64 ficam para hardware ARM64 real.

`bun build.ts ci-release --arch=aarch64`, com LTO, ainda não foi compilado.

O build release aarch64 morre com SIGILL sob o modelo `cortex-a53` do QEMU: o mimalloc que o Bun embute compila com `-march=armv8.1-a` e executa `casal` em `mi_process_init`. É nativo e fora do escopo; os builds com ASan rodam sob todos os modelos.

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




otimizar ics

Voltar em execeções tipo RepatchingPropertyInlineCache quando for implementar ics. 