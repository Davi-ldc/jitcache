(function() {
    function readProperty(object) {
        return object.x;
    }
    // Keep the load inside a non-inlined call, rather than hoisting it out of the loop.
    noInline(readProperty);

    var object = { x: 3 };

    function run() {
        var sum = 0;
        for (var i = 0; i < 2000000; ++i)
            sum += readProperty(object);
        return sum;
    }
    noInline(run);

    reportBench("inline-property-read", run, 2000000 * 3);
})();
