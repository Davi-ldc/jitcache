// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// Switches in imported images, each import twin-checked. A dense integer switch and a dense character switch load their
// jump table's base (SwitchTableBase), sparse integer and character switches compile to a list of comparisons, an inline
// switch_string compares atoms by address and jumps to each case through a ranked fixup (SwitchStringRankAtom and
// SwitchStringRankCase), and a switch_string past maximumInlineStringSwitchCaseCount (64) cases calls
// operationSwitchStringWithUnknownKeyType, which reads the image's string table. Each function runs 60 times, with atoms,
// with strings built at run time and with values no case names, before the producer's delta at exit captures it. The
// consumer checks that its images carry the SwitchTableBase, SwitchStringRankAtom and SwitchStringRankCase targets.
(function main(role, scratch, artifact, sequence) {
    function dense(n) {
        switch (n) {
        case 0: return "zero";
        case 1: return "one";
        case 2: return "two";
        case 3: return "three";
        case 4: return "four";
        case 5: return "five";
        case 6: return "six";
        case 7: return "seven";
        case 8: return "eight";
        case 9: return "nine";
        default: return "other";
        }
    }

    function sparse(n) {
        switch (n) {
        case -100000: return "far below";
        case 7: return "seven";
        case 5000: return "five thousand";
        case 100000: return "far above";
        case 2147483647: return "largest";
        default: return "none";
        }
    }

    function denseCharacters(c) {
        switch (c) {
        case "a": return 1;
        case "b": return 2;
        case "c": return 3;
        case "d": return 4;
        case "e": return 5;
        case "f": return 6;
        default: return 0;
        }
    }

    function sparseCharacters(c) {
        switch (c) {
        case "a": return 1;
        case "m": return 2;
        case "z": return 3;
        case "é": return 4;
        case "中": return 5;
        default: return 0;
        }
    }

    function inlineStrings(s) {
        switch (s) {
        case "alpha": return 1;
        case "bravo": return 2;
        case "charlie": return 3;
        case "delta": return 4;
        case "echo": return 5;
        case "foxtrot": return 6;
        default: return 0;
        }
    }

    // 70 cases, past the inline tree's 64.
    const largeCases = [];
    for (let i = 0; i < 70; ++i)
        largeCases.push(`case "large-key-${i}": return ${i + 1};`);
    const largeStrings = new Function("s", `switch (s) { ${largeCases.join(" ")} } return 0;`);

    const words = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf"];
    const built = word => [...word].join("");
    let total = 0;
    let text = "";
    for (let i = 0; i < 60; ++i) {
        text += dense(i % 12)[0];
        text += sparse([-100000, 7, 5000, 100000, 2147483647, 3, -1][i % 7])[0];
        total += denseCharacters("abcdefgh"[i % 8]);
        total += sparseCharacters("amzé中q"[i % 6]) * 10;
        total += inlineStrings(words[i % 7]) * 100;
        total += inlineStrings(built(words[(i + 3) % 7])) * 1000;
        total += largeStrings("large-key-" + (i % 75)) * 10000;
        total += largeStrings(i % 5 ? "large-key-1" : 1) * 100000;
    }
    print(text);
    print(total);

    // The case happened (HARNESS.md, Tests): the producer compiled every function, so each ran its import here.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const bodies = { dense, sparse, denseCharacters, sparseCharacters, inlineStrings, largeStrings };
        sections.expectImports(role, bodies);
        sections.expectTargets(role, { call: bodies }, [
            { site: "C12", kind: "SwitchTableBase", form: "Pointer" },
            { site: "C13, A16", kind: "SwitchStringRankAtom", form: "Pointer" },
            { site: "C13", kind: "SwitchStringRankCase", form: "Jump" },
        ]);
    }
})(...arguments);
