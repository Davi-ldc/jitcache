# Reconferir ao subir o pin do WebKit

- duble check names, numbers (N campos, N bytes) offset de `callq` mudam quando um campo muda de classe; checa `grep Options::`

## Histórico

- `954bf23450` →(6 semanas) `0d58b764f3`: nenhum mecanismo mudou; mudou nome, endereço de campo e um fato de concorrência.
- Um rename bateu em quatro docs de cinco; símbolo envelhece rápido, vamos ir cortando os não essenciais sempre que possível
- Uma option nova de bytes entrou por opcode que já existia; só a varredura por `Options::` em `jit/` a acha.
