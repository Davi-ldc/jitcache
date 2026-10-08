A ideia não é serializar dfg para algo transportavel e sim compartilhar entre UCBs que nem o jsc faz com o baselinejitdata


Meio que boa parte virou caminho normal ->
  - jitDataRegister materializado no prólogo, na entrada por OSR e no catch (dfg/DFGJITCompiler.cpp:168, dfg/DFGThunks.cpp:171).
  - DFG::JITData criado para todo plano em dfg/DFGJITFinalizer.cpp:66, com JSGlobalObject, deslocamento de pilha, contador de tier-up para o FTL,
    ArrayProfile dummy e a tabela de exits.
  - ICs de propriedade como moldes UnlinkedPropertyInlineCache instanciados no JITData na finalização (addPropertyInlineCache não tem ramo por modo,
    e a Bag do CommonData fica vazia por ASSERT).
  - Stubs de exit guardados no JITData por índice (dfg/DFGOSRExit.cpp:217), mesmo que no modo linkado o salto até eles ainda seja patcheado.

Histórico de desenvolvimento->

  - 2021-09/10 Saam Barati: unlinked baseline (m_unlinkedBaselineCode, BaselineJITData) e abre o bug do unlinked DFG com patches WIP.
  - 2022-03 a 2022-11 Yusuke Suzuki, treze commits: enum de modo, buffer de constantes via jitDataRegister, Data CallIC, OSR exit e invalidação
    unlinked, opção forceUnlinkedDFG, LinkerIR, CallLinkInfo unlinked, fase de validação, JSGlobalObject unlinked, watchpoints do realm. Para em
    novembro.
  - 2023-09 a 2024-09 a onda Data IC / Handler IC: "Get rid of CodeBlock dependency from DataIC", "Make each IC as one big handler", redesign do
    CallIC, Handler IC no baseline, jitDataRegister materializado no DFG (2024-07-11), Handler IC no DFG (2024-07-30), ArrayProfile dummy e contador
    de tier-up movidos para DFG::JITData (2024-08/09). É aqui que a infra do uDFG vira caminho normal.
  - 2024-01-02 kill-switch incondicional.
  - 2025 e 2026 JITData só aparece de raspão em commits de intrínsecos e RegExp; useHandlerICInFTL continua "not completed" e forçado a falso, que é
    a peça que o comentário do kill-switch parece esperar.


