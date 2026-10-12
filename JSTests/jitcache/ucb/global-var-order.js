// SPEC-ucb.md section 13.3, global-var-order.js, F10: a program with forty vars and twenty lets, then a program that
// redeclares several of them. A hashed declaration map iterates in the order of its layout, which the core carries
// (SPEC-ucb.codec.md, E4), so the global object's property order (Object.keys and the heap description the oracle
// compares) and the redeclaration error the second program throws equal the JITCache-off run when both programs import.
//
// The second program has to run to be captured, so every run first evaluates it in a realm of its own, where nothing
// conflicts, then evaluates the first program and the second one in the main realm. The Consumer imports both programs
// (statistics, imports); the second one's evaluation in the main realm meets its imported UCB through the CodeCache.
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4).
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);

    const names = (prefix, count) => Array.from({ length: count }, (_, i) => `${prefix}${(i * 7919 % 97).toString(36)}${i}`);
    const firstVars = names("gvoVar", 40);
    const firstLets = names("gvoLet", 20);
    const firstProgram = [
        `var ${firstVars.join(", ")};`,
        `let ${firstLets.map((name, i) => `${name} = ${i}`).join(", ")};`,
        `for (${firstVars[0]} = 0; ${firstVars[0]} < 3000; ++${firstVars[0]}) ${firstVars[1]} = ((${firstVars[1]} | 0) + ${firstVars[0]}) | 0;`,
        `${firstVars[1]};`,
    ].join("\n");

    // Lets that collide with the first program's vars and vars that collide with its lets, among enough fresh names that
    // both of the second program's declaration maps are hashed.
    const secondLets = [firstVars[3], firstVars[17], firstVars[29], ...names("gvoFreshLet", 12)];
    const secondVars = [firstLets[5], firstLets[11], ...names("gvoFreshVar", 12)];
    const secondProgram = [
        `let ${secondLets.map((name, i) => `${name} = ${i}`).join(", ")};`,
        `var ${secondVars.join(", ")};`,
        `for (${secondVars[2]} = 0; ${secondVars[2]} < 3000; ++${secondVars[2]}) ${secondVars[3]} = ((${secondVars[3]} | 0) + 2 * ${secondVars[2]}) | 0;`,
        `${secondVars[3]};`,
    ].join("\n");

    function evaluate(label, evaluator, text)
    {
        const before = t.statistics();
        const value = evaluator(text);
        const after = t.statistics();
        if (t.imports)
            t.expect(label, t.delta(before, after), { imports: [">=", 1] });
        return value;
    }

    evaluate("the second program in its own realm", runString, secondProgram);
    print(`first program ${evaluate("the first program", loadString, firstProgram)}`);
    try {
        loadString(secondProgram);
        print("the second program ran without a redeclaration error");
    } catch (error) {
        print(`second program: ${error.name}: ${error.message}`);
    }
    print(Object.keys(globalThis).filter(name => name.startsWith("gvo")).join(" "));
    t.finish();
})(...arguments);
