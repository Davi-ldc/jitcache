# Exemplo de microbenchmark

Um único workload de acesso nomeado a `object.x`, com um único formato de objeto. A leitura fica numa função marcada com `noInline`, impedindo que o chamador incorpore o corpo e retire a leitura do loop. O tempo inclui a chamada dessa função, não só o acesso à propriedade. Workload e harness adaptados de `JSTests/threads/bench/` do PR 249 de `oven-sh/WebKit` (`3a14f2a821ac56fcb01d1c765200be7e9dfdb458`).

Na raiz física do checkout WSL, usando o Release local já compilado:

```bash
JSC="${JITCACHE_BUILD_ROOT:-$HOME/collo-local/build/jitcache}/linux-$(uname -m)-release-local/deps/WebKit/bin/jsc"
"$JSC" JSTests/jitcache/bench/harness.js JSTests/jitcache/bench/inline-property-read.js
```

O harness faz 20 chamadas de aquecimento e 50 chamadas medidas, verificando o checksum em todas. Esse aquecimento fixo não comprova que todos os tiers terminaram de compilar. A saída `BENCH inline-property-read <ms>` contém o tempo total das 50 chamadas medidas; não mede startup nem instalação de baseline serializado. É apenas um exemplo executável: sem runner, baseline ou gate de regressão.
