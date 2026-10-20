#include "config.h"
#include "JITCacheTwinsHarness.h"

#if ENABLE(JITCACHE_TWINS)

#include "AbstractModuleRecord.h"
#include "BigIntObject.h"
#include "BooleanObject.h"
#include "BrandedStructure.h"
#include "CommonIdentifiers.h"
#include "CyclicModuleRecord.h"
#include "DateInstance.h"
#include "DeferGC.h"
#include "FunctionExecutable.h"
#include "GetterSetter.h"
#include "InternalFieldTuple.h"
#include "InternalFunction.h"
#include "JSArrayBuffer.h"
#include "JSArrayBufferView.h"
#include "JSArrayIterator.h"
#include "JSAsyncDisposableStack.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSAsyncGenerator.h"
#include "JSBoundFunction.h"
#include "JSBoundFunctionInlines.h"
#include "JSCInlines.h"
#include "JSDisposableStack.h"
#include "JSFinalizationRegistry.h"
#include "JSFunction.h"
#include "JSFunctionInlines.h"
#include "JSGenerator.h"
#include "JSGlobalLexicalEnvironment.h"
#include "JSIteratorHelper.h"
#include "JSLexicalEnvironment.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSModuleEnvironment.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "JSPromise.h"
#include "JSRegExpStringIterator.h"
#include "JSSet.h"
#include "JSSetIterator.h"
#include "JSStringIterator.h"
#include "JSWeakMap.h"
#include "JSWeakObjectRef.h"
#include "JSWeakSet.h"
#include "JSWithScope.h"
#include "JSWrapForValidIterator.h"
#include "JSWrapperObject.h"
#include "MarkedVector.h"
#include "ModuleRegistryEntry.h"
#include "NumberObject.h"
#include "ProxyObject.h"
#include "RegExp.h"
#include "RegExpObject.h"
#include "StringObject.h"
#include "Symbol.h"
#include "SymbolObject.h"
#include "SymbolTable.h"
#include "VM.h"
#include "YarrFlags.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HexNumber.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

// The description of the heap JavaScript can reach, which the runner compares between each JITCache run and the run of
// the same script with JITCache off. Closure variables, internal slots and private names are invisible to reflection,
// and a walk written in JavaScript would run getters and proxy traps, so the engine writes the description itself. It is
// a function of what JavaScript can observe: it holds no address, structure, storage layout or string representation,
// and no cell JavaScript cannot reach, such as an executable, a CodeBlock or JIT code. Two runs that perform the same
// JavaScript operations therefore describe alike, whatever tier each body ran in and whatever its ICs held.

namespace JSC::JITCache {

namespace HeapDescriptionInternal {

static String jsonString(const String& string)
{
    StringBuilder builder;
    builder.appendQuotedJSONString(string.isNull() ? emptyString() : string);
    return builder.toString();
}

// Numbers in shortest round-trip form, with -0, NaN and the infinities spelled out and no int32 or double distinction.
static String numberText(double number)
{
    if (std::isnan(number))
        return "NaN"_s;
    if (std::isinf(number))
        return number > 0 ? "Infinity"_s : "-Infinity"_s;
    if (!number)
        return std::signbit(number) ? "-0"_s : "0"_s;
    return String::number(number);
}

static ASCIILiteral flagText(bool value)
{
    return value ? "1"_s : "0"_s;
}

// One walk over the graph, breadth first. Every cell takes the next ordinal the first time the walk meets it, and the
// output is one line per cell in ordinal order, each followed by one line per edge. Every object's line starts with its
// class and its callable, constructor and extensible flags, the any-object line, to which describeObject appends what
// the object's class adds.
class HeapDescription {
    WTF_MAKE_NONCOPYABLE(HeapDescription);
public:
    // Whether the walk follows the edges of the global object, its JSGlobalProxy and its global lexical environment.
    // From the default roots it does. From given roots it writes each of the three that is no root with its line and no
    // edges: every function's scope chain ends there, so every root reaches them, and Bun's global object holds
    // per-process state.
    enum class GlobalScope : bool { Walked, WithoutEdges };

    // Wherever the walk follows the global object's edges, directly or through its JSGlobalProxy, it leaves out the global
    // object's own property `arguments`, where the jsc shell puts the run's role and paths, so a value only `arguments`
    // reaches changes no description.
    explicit HeapDescription(JSGlobalObject& globalObject)
        : m_globalObject(globalObject)
        , m_vm(globalObject.vm())
    {
    }

    // Called once per HeapDescription. Null when reifying or reading a property threw; the exception stays on the VM.
    String describe(std::span<const JSValue> roots, GlobalScope globalScope)
    {
        ASSERT(m_records.isEmpty());
        if (globalScope == GlobalScope::WithoutEdges) {
            std::array<JSCell*, 3> globalScopeCells { &m_globalObject, m_globalObject.globalThis(), m_globalObject.globalLexicalEnvironment() };
            for (JSCell* cell : globalScopeCells) {
                bool isRoot = std::ranges::any_of(roots, [&](JSValue root) {
                    return root.isCell() && root.asCell() == cell;
                });
                if (cell && !isRoot && !m_cellsWithoutEdges.contains(cell))
                    m_cellsWithoutEdges.append(cell);
            }
        }

        Line rootLine { text("roots"_s) };
        for (JSValue root : roots) {
            if (!meet(root, rootLine))
                return { };
        }
        // Strong edges first, then the weak entries whose keys the walk reached, until a round meets no cell.
        while (true) {
            while (m_nextToDescribe < m_records.size()) {
                if (!describeCell(m_nextToDescribe++))
                    return { };
            }
            size_t cellsBefore = m_records.size();
            if (!meetReachedWeakEntries())
                return { };
            if (m_records.size() == cellsBefore)
                break;
        }
        return output(rootLine);
    }

private:
    // An item of a line: text, or a cell that the line names by its ordinal. A weak edge never gives a cell its ordinal,
    // since whether a cell only weak edges reach is still alive depends on the collector: a weak target the walk did not
    // reach writes <unreached>, and a finalization registry's held value or token that it did not reach writes <cell>.
    enum class ItemKind : uint8_t { Text, Cell, WeakCell, ListedCell };
    struct Item {
        ItemKind kind;
        String text;
        JSCell* cell { nullptr };
    };
    using Line = Vector<Item, 6>;

    struct Record {
        JSCell* cell { nullptr };
        String line;
        Vector<Line> edges;
        // A weak map's or weak set's entries, the value empty for a set, and the lines of those whose keys the walk reached.
        bool isWeakContainer { false };
        bool weakEntriesHaveValues { false };
        Vector<std::pair<JSCell*, JSValue>> weakEntries;
        Vector<bool> weakEntryMet;
        Vector<std::pair<unsigned, Line>> weakEntryLines; // by the key's ordinal
        // A finalization registry's registrations: the held value and the unregister token.
        Vector<std::pair<Item, Item>> registrations;
    };

    static Item text(String string) { return { ItemKind::Text, WTF::move(string), nullptr }; }
    static Item cellItem(ItemKind kind, JSCell* cell) { return { kind, { }, cell }; }

    void reach(JSCell* cell)
    {
        auto result = m_ordinals.add(cell, static_cast<unsigned>(m_records.size()));
        if (result.isNewEntry)
            m_records.append(Record { cell });
    }

    // A primitive's text, or nothing for a cell: undefined, null and booleans; numbers; BigInts in decimal; strings as
    // JSON string literals, every lone surrogate escaped; the empty value of an uninitialized binding as <empty>.
    std::optional<String> primitiveText(JSValue value, bool& threw)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        threw = false;
        if (!value)
            return "<empty>"_s;
        if (value.isUndefined())
            return "undefined"_s;
        if (value.isNull())
            return "null"_s;
        if (value.isBoolean())
            return value.asBoolean() ? "true"_s : "false"_s;
        if (value.isNumber())
            return numberText(value.asNumber());
        if (value.isBigInt()) {
            String digits = value.toWTFString(&m_globalObject);
            threw = !!scope.exception();
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            return makeString(digits, 'n');
        }
        if (value.isString()) {
            String string = asString(value)->value(&m_globalObject);
            threw = !!scope.exception();
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            return jsonString(string);
        }
        return std::nullopt;
    }

    // Appends what a value writes where the walk meets it: a primitive, or the cell's ordinal, which the cell takes the
    // first time the walk meets it. False when an allocation threw.
    bool meet(JSValue value, Line& line)
    {
        bool threw = false;
        if (auto primitive = primitiveText(value, threw)) {
            line.append(text(WTF::move(*primitive)));
            return true;
        }
        if (threw)
            return false;
        reach(value.asCell());
        line.append(cellItem(ItemKind::Cell, value.asCell()));
        return true;
    }

    // A value a finalization registry lists without walking it.
    bool listedItem(JSValue value, Item& item)
    {
        bool threw = false;
        if (auto primitive = primitiveText(value, threw)) {
            item = text(WTF::move(*primitive));
            return true;
        }
        if (threw)
            return false;
        item = cellItem(ItemKind::ListedCell, value.asCell());
        return true;
    }

    bool describeCell(unsigned ordinal)
    {
        JSCell* cell = m_records[ordinal].cell;
        if (auto* symbol = dynamicDowncast<Symbol>(cell)) {
            m_records[ordinal].line = symbolLine(*symbol);
            return true;
        }
        auto* object = dynamicDowncast<JSObject>(cell);
        if (!object) {
            m_records[ordinal].line = makeString("cell "_s, cell->classInfo()->className);
            return true;
        }
        // From given roots, the global scope's cells that are no root: the global lexical environment writes its scope
        // line, and the global object and its JSGlobalProxy, whose classes add nothing to an object's line, the
        // any-object line.
        if (m_cellsWithoutEdges.contains(cell)) {
            if (is<JSGlobalLexicalEnvironment>(object))
                m_records[ordinal].line = scopeLine("global-lexical"_s);
            else {
                StringBuilder line;
                appendObjectLine(line, *object);
                m_records[ordinal].line = line.toString();
            }
            return true;
        }
        // A scope is no object JavaScript reaches: its line and edges describe it in full, its variables sorted by name,
        // which keeps the symbol table's hash order out of the description.
        if (auto* withScope = dynamicDowncast<JSWithScope>(object))
            return describeWithScope(ordinal, *withScope);
        if (auto* environment = dynamicDowncast<JSLexicalEnvironment>(object))
            return describeScope(ordinal, *environment, scopeKind(*environment));
        if (auto* globalLexicalEnvironment = dynamicDowncast<JSGlobalLexicalEnvironment>(object))
            return describeScope(ordinal, *globalLexicalEnvironment, "global-lexical"_s);
        return describeObject(ordinal, *object);
    }

    static String symbolLine(Symbol& symbol)
    {
        SymbolImpl& uid = symbol.uid();
        String description = uid.isNullSymbol() ? "undefined"_s : jsonString(String { &uid });
        StringBuilder line;
        line.append("symbol "_s, description);
        // A registered symbol's Symbol.for key is its description.
        if (uid.isRegistered())
            line.append(" for "_s, description);
        if (uid.isPrivate())
            line.append(" private"_s);
        return line.toString();
    }

    static ASCIILiteral scopeKind(JSLexicalEnvironment& environment)
    {
        if (is<JSModuleEnvironment>(&environment))
            return "module"_s;
        switch (environment.symbolTable()->scopeType()) {
        case SymbolTable::ScopeType::VarScope:
            return "var"_s;
        case SymbolTable::ScopeType::GlobalLexicalScope:
            return "global-lexical"_s;
        case SymbolTable::ScopeType::LexicalScope:
            return "lexical"_s;
        case SymbolTable::ScopeType::CatchScope:
        case SymbolTable::ScopeType::CatchScopeWithSimpleParameter:
            return "catch"_s;
        case SymbolTable::ScopeType::FunctionNameScope:
            return "function-name"_s;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return "lexical"_s;
    }

    bool appendNextScope(JSScope& scopeObject, Vector<Line>& edges)
    {
        JSScope* next = scopeObject.next();
        Line line { text("[[NextScope]]"_s) };
        if (!meet(next ? JSValue(next) : jsNull(), line))
            return false;
        edges.append(WTF::move(line));
        return true;
    }

    bool describeWithScope(unsigned ordinal, JSWithScope& withScope)
    {
        Vector<Line> edges;
        Line object { text("[[Object]]"_s) };
        if (!meet(withScope.object(), object))
            return false;
        edges.append(WTF::move(object));
        if (!appendNextScope(withScope, edges))
            return false;
        m_records[ordinal].line = scopeLine("with"_s);
        m_records[ordinal].edges = WTF::move(edges);
        return true;
    }

    // A scope's line, in place of the any-object line.
    static String scopeLine(ASCIILiteral kind)
    {
        return makeString("scope "_s, kind);
    }

    // Each variable the symbol table places in the scope, sorted by name, then the next scope.
    template<typename ScopeType>
    bool describeScope(unsigned ordinal, ScopeType& scopeObject, ASCIILiteral kind)
    {
        Vector<std::pair<String, ScopeOffset>> variables;
        {
            SymbolTable* table = scopeObject.symbolTable();
            ConcurrentJSLocker locker(table->m_lock);
            for (auto iterator = table->begin(locker), end = table->end(locker); iterator != end; ++iterator) {
                if (iterator->value.isNull())
                    continue;
                VarOffset offset = iterator->value.varOffset();
                if (!offset.isScope())
                    continue;
                variables.append({ String { iterator->key.get() }, offset.scopeOffset() });
            }
        }
        std::stable_sort(variables.begin(), variables.end(), [](const auto& a, const auto& b) {
            return codePointCompareLessThan(a.first, b.first);
        });

        Vector<Line> edges;
        for (auto& [name, offset] : variables) {
            Line line { text("variable"_s), text(jsonString(name)), text("="_s) };
            if (!meet(scopeObject.variableAt(offset).get(), line))
                return false;
            edges.append(WTF::move(line));
        }
        if (!appendNextScope(scopeObject, edges))
            return false;
        m_records[ordinal].line = scopeLine(kind);
        m_records[ordinal].edges = WTF::move(edges);
        return true;
    }

    static String attributesText(unsigned attributes, bool isAccessor)
    {
        // The three attributes JavaScript sees; an accessor has no writable one.
        std::array<char, 3> letters {
            !isAccessor && !(attributes & PropertyAttribute::ReadOnly) ? 'w' : '-',
            !(attributes & PropertyAttribute::DontEnum) ? 'e' : '-',
            !(attributes & PropertyAttribute::DontDelete) ? 'c' : '-',
        };
        return String::fromLatin1(std::span<const char> { letters });
    }

    // The own properties in the order getOwnPropertyNames gives them, private names included, each read with a VMInquiry
    // slot, which calls no getter, trap or host accessor.
    bool appendOwnProperties(JSObject& object, bool skipsIndexedProperties, Vector<Line>& edges)
    {
        VM& vm = m_vm;
        auto scope = DECLARE_THROW_SCOPE(vm);

        PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Include);
        object.methodTable()->getOwnPropertyNames(&object, &m_globalObject, names, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, false);

        for (unsigned nameIndex = 0; nameIndex < names.size(); ++nameIndex) {
            const Identifier& name = names[nameIndex];
            std::optional<uint32_t> index = parseIndex(name);
            if (index && skipsIndexedProperties)
                continue;
            // The global object's own `arguments` stays out whichever way the walk meets it: directly, or through the
            // JSGlobalProxy its globalThis property holds, which forwards every own-property read to it.
            if ((&object == &m_globalObject || &object == m_globalObject.globalThis()) && name == vm.propertyNames->arguments)
                continue;

            Line line { text("property"_s) };
            if (name.isSymbol()) {
                // The symbol cell JavaScript would get for this key, the one the VM keeps for its uid.
                if (!meet(Symbol::create(vm, static_cast<SymbolImpl&>(*name.impl())), line))
                    return false;
            } else
                line.append(text(jsonString(name.string())));

            PropertySlot slot(&object, PropertySlot::InternalMethodType::VMInquiry, &vm);
            bool found = index
                ? object.methodTable()->getOwnPropertySlotByIndex(&object, &m_globalObject, *index, slot)
                : object.methodTable()->getOwnPropertySlot(&object, &m_globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, false);
            if (!found) {
                line.append(text("<opaque>"_s));
                edges.append(WTF::move(line));
                continue;
            }

            line.append(text(attributesText(slot.attributes(), slot.isAccessor())));
            if (slot.isValue()) {
                line.append(text("="_s));
                if (!meet(slot.getValue(&m_globalObject, name), line))
                    return false;
            } else if (slot.isAccessor()) {
                GetterSetter* accessor = slot.getterSetter();
                line.append(text("get"_s));
                if (!meet(accessor->isGetterNull() ? jsUndefined() : JSValue(accessor->getter()), line))
                    return false;
                line.append(text("set"_s));
                if (!meet(accessor->isSetterNull() ? jsUndefined() : JSValue(accessor->setter()), line))
                    return false;
            } else {
                line.append(text("="_s));
                line.append(text("<custom>"_s));
            }
            edges.append(WTF::move(line));
        }
        return true;
    }

    // The value of the binding an export resolves to, read from its module environment so that nothing is materialized;
    // an empty binding is the empty value. Nothing when the export resolves to no binding of an environment.
    static std::optional<JSValue> exportBindingValue(const AbstractModuleRecord::Resolution& resolution)
    {
        if (resolution.type != AbstractModuleRecord::Resolution::Type::Resolved || !resolution.moduleRecord)
            return std::nullopt;
        JSModuleEnvironment* environment = resolution.moduleRecord->moduleEnvironmentMayBeNull();
        if (!environment)
            return std::nullopt;
        ScopeOffset offset;
        {
            SymbolTable* table = environment->symbolTable();
            ConcurrentJSLocker locker(table->m_lock);
            auto iterator = table->find(locker, resolution.localName.impl());
            if (iterator == table->end(locker) || iterator->value.isNull() || !iterator->value.varOffset().isScope())
                return std::nullopt;
            offset = iterator->value.varOffset().scopeOffset();
        }
        return environment->variableAt(offset).get();
    }

    bool appendExports(JSModuleNamespaceObject& namespaceObject, Vector<Line>& edges)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);

        if (exportsNeedEvaluation(namespaceObject)) {
            edges.append(Line { text("exports"_s), text("<unevaluated>"_s) });
            return true;
        }

        PropertyNameArrayBuilder names(m_vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        namespaceObject.methodTable()->getOwnPropertyNames(&namespaceObject, &m_globalObject, names, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, false);

        AbstractModuleRecord* moduleRecord = namespaceObject.moduleRecord();
        for (unsigned nameIndex = 0; nameIndex < names.size(); ++nameIndex) {
            const Identifier& name = names[nameIndex];
            AbstractModuleRecord::Resolution resolution = moduleRecord->resolveExport(&m_globalObject, name);
            RETURN_IF_EXCEPTION(scope, false);
            Line line { text("export"_s), text(jsonString(name.string())), text("="_s) };
            std::optional<JSValue> value = exportBindingValue(resolution);
            if (!value)
                line.append(text("<opaque>"_s));
            else if (!meet(*value, line))
                return false;
            edges.append(WTF::move(line));
        }
        return true;
    }

    bool appendEdge(Vector<Line>& edges, ASCIILiteral label, JSValue value)
    {
        Line line { text(label) };
        if (!meet(value, line))
            return false;
        edges.append(WTF::move(line));
        return true;
    }

    // The internal fields of a JSInternalFieldObjectImpl that describeObject's other cases leave out (iterators,
    // generators). Nothing when the object is not of the class; false when an allocation threw.
    template<typename CellType>
    std::optional<bool> appendInternalFieldsOf(JSObject& object, Vector<Line>& edges)
    {
        auto* cell = dynamicDowncast<CellType>(&object);
        if (!cell)
            return std::nullopt;
        // Through the base, since several classes hide its indexed accessor behind one taking their Field enumeration.
        using Fields = JSInternalFieldObjectImpl<CellType::numberOfInternalFields>;
        const Fields& fields = *cell;
        for (unsigned index = 0; index < Fields::numberOfInternalFields; ++index) {
            Line line { text("[[InternalField]]"_s), text(String::number(index)), text("="_s) };
            if (!meet(fields.internalField(index).get(), line))
                return false;
            edges.append(WTF::move(line));
        }
        return true;
    }

    template<typename... CellTypes>
    bool appendInternalFields(JSObject& object, Vector<Line>& edges)
    {
        // The first class the object is of stops the fold.
        std::optional<bool> result;
        static_cast<void>(((result = appendInternalFieldsOf<CellTypes>(object, edges)) || ...));
        return result.value_or(true);
    }

    // The five classes whose object wraps a primitive. JSWrapperObject declares no ClassInfo of its own, so a cast to it
    // would test its base's and accept every JSNonFinalObject; each subclass declares its own.
    static JSWrapperObject* primitiveWrapper(JSObject& object)
    {
        if (auto* number = dynamicDowncast<NumberObject>(&object))
            return number;
        if (auto* string = dynamicDowncast<StringObject>(&object))
            return string;
        if (auto* boolean = dynamicDowncast<BooleanObject>(&object))
            return boolean;
        if (auto* symbol = dynamicDowncast<SymbolObject>(&object))
            return symbol;
        if (auto* bigInt = dynamicDowncast<BigIntObject>(&object))
            return bigInt;
        return nullptr;
    }

    // A deferred namespace runs its module's evaluation at the first look at its keys, unless the module's cycle already
    // evaluated without an error (JSModuleNamespaceObject::ensureDeferredNamespaceEvaluation). JavaScript cannot list such
    // a namespace's exports without running that evaluation, so the walk lists them only once it ran. The namespace's own
    // toStringTag tells it apart, since its deferral has no accessor.
    bool exportsNeedEvaluation(JSModuleNamespaceObject& namespaceObject)
    {
        JSValue tag = namespaceObject.getDirect(m_vm, m_vm.propertyNames->toStringTagSymbol);
        // finishCreation stores the tag as a flat string, which needs no resolution.
        StringImpl* tagImpl = tag.isString() ? asString(tag)->tryGetValueImpl() : nullptr;
        if (!tagImpl || !WTF::equal(tagImpl, "Deferred Module"_s))
            return false;
        auto* cyclic = dynamicDowncast<CyclicModuleRecord>(namespaceObject.moduleRecord());
        if (!cyclic)
            return true;
        CyclicModuleRecord* root = cyclic->cycleRoot() ? cyclic->cycleRoot() : cyclic;
        return root->status() != CyclicModuleRecord::Status::Evaluated || root->evaluationError();
    }

    // The any-object line. Extensibility is the structure's, so no trap runs.
    static void appendObjectLine(StringBuilder& line, JSObject& object)
    {
        line.append("object "_s, object.classInfo()->className, " callable="_s, flagText(object.isCallable()),
            " constructor="_s, flagText(object.isConstructor()), " extensible="_s, flagText(object.isStructureExtensible()));
    }

    bool describeObject(unsigned ordinal, JSObject& object)
    {
        VM& vm = m_vm;
        auto scope = DECLARE_THROW_SCOPE(vm);

        StringBuilder line;
        appendObjectLine(line, object);
        Vector<Line> edges;
        auto* view = dynamicDowncast<JSArrayBufferView>(&object);

        if (auto* proxy = dynamicDowncast<ProxyObject>(&object)) {
            // Target and handler in place of the prototype and own properties, which only traps could read; both are null
            // once the proxy is revoked, as JavaScript sees them.
            bool revoked = proxy->isRevoked();
            if (!appendEdge(edges, "[[ProxyTarget]]"_s, revoked ? jsNull() : JSValue(proxy->target())))
                return false;
            if (!appendEdge(edges, "[[ProxyHandler]]"_s, revoked ? jsNull() : proxy->handler()))
                return false;
        } else {
            if (!appendEdge(edges, "[[Prototype]]"_s, object.getPrototypeDirect()))
                return false;
            if (auto* namespaceObject = dynamicDowncast<JSModuleNamespaceObject>(&object)) {
                if (!appendExports(*namespaceObject, edges))
                    return false;
            } else if (!appendOwnProperties(object, !!view, edges))
                return false;
        }

        if (auto* function = dynamicDowncast<JSFunction>(&object)) {
            if (auto* bound = dynamicDowncast<JSBoundFunction>(function)) {
                if (!appendEdge(edges, "[[BoundTargetFunction]]"_s, bound->targetFunction()))
                    return false;
                if (!appendEdge(edges, "[[BoundThis]]"_s, bound->boundThis()))
                    return false;
                bool succeeded = true;
                unsigned argumentIndex = 0;
                bound->forEachBoundArg([&](JSValue argument) {
                    Line argumentLine { text("[[BoundArgument]]"_s), text(String::number(argumentIndex++)), text("="_s) };
                    succeeded = meet(argument, argumentLine);
                    if (!succeeded)
                        return IterationStatus::Done;
                    edges.append(WTF::move(argumentLine));
                    return IterationStatus::Continue;
                });
                if (!succeeded)
                    return false;
            } else if (!function->isHostFunction()) {
                FunctionExecutable* executable = function->jsExecutable();
                if (executable->isBuiltinFunction())
                    line.append(" builtin "_s, jsonString(executable->name().string()));
                else
                    line.append(" source "_s, jsonString(executable->sourceURL()), ' ', executable->source().startOffset(), '-', executable->source().endOffset());
                if (!appendEdge(edges, "[[Scope]]"_s, function->scope()))
                    return false;
            } else
                line.append(" name "_s, jsonString(function->name(vm)));
        } else if (auto* internalFunction = dynamicDowncast<InternalFunction>(&object))
            line.append(" name "_s, jsonString(internalFunction->name()));
        else if (auto* map = dynamicDowncast<JSMap>(&object)) {
            if (map->storage()) {
                auto& storage = map->storageRef();
                for (JSMap::Helper::Entry entry = 0;; ++entry) {
                    auto result = JSMap::Helper::transitAndNext(vm, storage, entry);
                    if (!result.storage)
                        break;
                    Line entryLine { text("entry"_s) };
                    if (!meet(result.key, entryLine))
                        return false;
                    entryLine.append(text("=>"_s));
                    if (!meet(result.value, entryLine))
                        return false;
                    edges.append(WTF::move(entryLine));
                    entry = result.entry;
                }
            }
        } else if (auto* set = dynamicDowncast<JSSet>(&object)) {
            if (set->storage()) {
                auto& storage = set->storageRef();
                for (JSSet::Helper::Entry entry = 0;; ++entry) {
                    auto result = JSSet::Helper::transitAndNext(vm, storage, entry);
                    if (!result.storage)
                        break;
                    Line entryLine { text("entry"_s) };
                    if (!meet(result.key, entryLine))
                        return false;
                    edges.append(WTF::move(entryLine));
                    entry = result.entry;
                }
            }
        } else if (auto* weakMap = dynamicDowncast<JSWeakMap>(&object)) {
            MarkedArgumentBuffer snapshot;
            weakMap->takeSnapshot(snapshot);
            Record& record = m_records[ordinal];
            record.isWeakContainer = true;
            record.weakEntriesHaveValues = true;
            for (size_t index = 0; index + 1 < snapshot.size(); index += 2)
                record.weakEntries.append({ snapshot.at(index).asCell(), snapshot.at(index + 1) });
            record.weakEntryMet.grow(record.weakEntries.size());
            record.weakEntryMet.fill(false);
        } else if (auto* weakSet = dynamicDowncast<JSWeakSet>(&object)) {
            MarkedArgumentBuffer snapshot;
            weakSet->takeSnapshot(snapshot);
            Record& record = m_records[ordinal];
            record.isWeakContainer = true;
            for (size_t index = 0; index < snapshot.size(); ++index)
                record.weakEntries.append({ snapshot.at(index).asCell(), JSValue() });
            record.weakEntryMet.grow(record.weakEntries.size());
            record.weakEntryMet.fill(false);
        } else if (auto* weakRef = dynamicDowncast<JSWeakObjectRef>(&object)) {
            // deref is the only reader the class has; it keeps the target alive for the rest of the job, as
            // WeakRef.prototype.deref does.
            Line target { text("[[WeakRefTarget]]"_s), cellItem(ItemKind::WeakCell, weakRef->deref(vm)) };
            edges.append(WTF::move(target));
        } else if (auto* registry = dynamicDowncast<JSFinalizationRegistry>(&object)) {
            if (!appendEdge(edges, "[[CleanupCallback]]"_s, registry->internalField(JSFinalizationRegistry::Field::Callback).get()))
                return false;
            Vector<std::pair<JSValue, JSCell*>> registrations;
            {
                Locker locker { registry->cellLock() };
                for (auto& live : registry->liveRegistrations(locker))
                    registrations.append({ live.heldValue, live.unregisterToken });
                for (auto& dead : registry->deadRegistrations(locker))
                    registrations.append({ dead.heldValue, dead.unregisterToken });
            }
            for (auto& [heldValue, token] : registrations) {
                Item heldItem = text({ });
                Item tokenItem = text("undefined"_s);
                if (!listedItem(heldValue, heldItem))
                    return false;
                if (token)
                    tokenItem = cellItem(ItemKind::ListedCell, token);
                m_records[ordinal].registrations.append({ WTF::move(heldItem), WTF::move(tokenItem) });
            }
        } else if (auto* promise = dynamicDowncast<JSPromise>(&object)) {
            switch (promise->status()) {
            case JSPromise::Status::Pending:
                line.append(" status pending"_s);
                break;
            case JSPromise::Status::Fulfilled:
                line.append(" status fulfilled"_s);
                break;
            case JSPromise::Status::Rejected:
                line.append(" status rejected"_s);
                break;
            }
            // Its result, not its reactions, which JavaScript observes only through what they run once it settles.
            if (!appendEdge(edges, "[[PromiseResult]]"_s, promise->result()))
                return false;
        } else if (JSWrapperObject* wrapper = primitiveWrapper(object)) {
            if (!appendEdge(edges, "[[PrimitiveValue]]"_s, wrapper->internalValue()))
                return false;
        } else if (auto* date = dynamicDowncast<DateInstance>(&object))
            line.append(" time "_s, numberText(date->internalNumber()));
        else if (auto* regExpObject = dynamicDowncast<RegExpObject>(&object)) {
            RegExp* regExp = regExpObject->regExp();
            auto flags = Yarr::flagsString(regExp->flags());
            line.append(" pattern "_s, jsonString(regExp->pattern()), " flags "_s, jsonString(String { flags.span() }));
        } else if (auto* arrayBuffer = dynamicDowncast<JSArrayBuffer>(&object)) {
            ArrayBuffer* buffer = arrayBuffer->impl();
            line.append(" shared="_s, flagText(buffer->isShared()), " detached="_s, flagText(buffer->isDetached()), " byteLength="_s, buffer->byteLength());
            StringBuilder bytes;
            for (uint8_t byte : buffer->span())
                bytes.append(hex(byte, 2, Lowercase));
            Line bytesLine { text("bytes"_s), text(bytes.toString()) };
            edges.append(WTF::move(bytesLine));
        } else if (view) {
            line.append(" byteOffset="_s, view->byteOffset(), " length="_s, view->length());
            // The buffer, in place of the indexed own properties appendOwnProperties left out.
            JSArrayBuffer* buffer = view->possiblySharedJSBuffer(&m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            if (!appendEdge(edges, "[[ViewedArrayBuffer]]"_s, buffer ? JSValue(buffer) : jsNull()))
                return false;
        } else if (!appendInternalFields<JSArrayIterator, JSMapIterator, JSSetIterator, JSStringIterator, JSRegExpStringIterator,
            JSIteratorHelper, JSWrapForValidIterator, JSGenerator, JSAsyncGenerator, JSAsyncFunctionGenerator, JSDisposableStack,
            JSAsyncDisposableStack, AbstractModuleRecord, InternalFieldTuple>(object, edges))
            return false;

        m_records[ordinal].line = line.toString();
        m_records[ordinal].edges = WTF::move(edges);
        return true;
    }

    // One round of weak entries: in each weak container, in ordinal order, the entries whose key the walk has reached and
    // that no earlier round met, in their keys' ordinal order. Their values are walked like any strong edge.
    bool meetReachedWeakEntries()
    {
        size_t recordCount = m_records.size();
        for (size_t ordinal = 0; ordinal < recordCount; ++ordinal) {
            if (!m_records[ordinal].isWeakContainer)
                continue;
            Vector<std::pair<unsigned, size_t>> reached; // the key's ordinal, the entry's index
            for (size_t index = 0; index < m_records[ordinal].weakEntries.size(); ++index) {
                if (m_records[ordinal].weakEntryMet[index])
                    continue;
                auto iterator = m_ordinals.find(m_records[ordinal].weakEntries[index].first);
                if (iterator != m_ordinals.end())
                    reached.append({ iterator->value, index });
            }
            std::ranges::sort(reached);
            for (auto [keyOrdinal, index] : reached) {
                auto [key, value] = m_records[ordinal].weakEntries[index];
                Line line { text("weak-entry"_s), cellItem(ItemKind::Cell, key) };
                if (m_records[ordinal].weakEntriesHaveValues) {
                    line.append(text("=>"_s));
                    if (!meet(value, line))
                        return false;
                }
                m_records[ordinal].weakEntryMet[index] = true;
                m_records[ordinal].weakEntryLines.append({ keyOrdinal, WTF::move(line) });
            }
        }
        return true;
    }

    String itemText(const Item& item) const
    {
        if (item.kind == ItemKind::Text)
            return item.text;
        auto iterator = item.cell ? m_ordinals.find(item.cell) : m_ordinals.end();
        if (iterator != m_ordinals.end())
            return makeString('#', iterator->value);
        // A strong edge always gives its cell an ordinal.
        ASSERT(item.kind != ItemKind::Cell);
        return item.kind == ItemKind::WeakCell ? "<unreached>"_s : "<cell>"_s;
    }

    void appendLine(StringBuilder& output, const Line& line) const
    {
        output.append("  "_s);
        for (size_t index = 0; index < line.size(); ++index)
            output.append(index ? " "_s : ""_s, itemText(line[index]));
        output.append('\n');
    }

    String output(const Line& rootLine)
    {
        // The private brands each object carries, tested against every private symbol the walk reached, in their ordinal
        // order: BrandedStructure exposes its brands only through checkBrand.
        Vector<Symbol*> privateSymbols;
        for (auto& record : m_records) {
            if (auto* symbol = dynamicDowncast<Symbol>(record.cell); symbol && symbol->uid().isPrivate())
                privateSymbols.append(symbol);
        }

        StringBuilder output;
        output.append(rootLine.first().text);
        for (size_t index = 1; index < rootLine.size(); ++index)
            output.append(' ', itemText(rootLine[index]));
        output.append('\n');

        for (size_t ordinal = 0; ordinal < m_records.size(); ++ordinal) {
            Record& record = m_records[ordinal];
            output.append('#', ordinal, ' ', record.line, '\n');
            for (auto& edge : record.edges)
                appendLine(output, edge);

            std::ranges::sort(record.weakEntryLines, { }, [](const auto& entry) { return entry.first; });
            for (auto& [keyOrdinal, line] : record.weakEntryLines)
                appendLine(output, line);

            Vector<String> registrations;
            for (auto& [heldItem, tokenItem] : record.registrations)
                registrations.append(makeString("registration held "_s, itemText(heldItem), " token "_s, itemText(tokenItem)));
            std::ranges::sort(registrations, [](const String& a, const String& b) { return codePointCompareLessThan(a, b); });
            for (auto& registration : registrations)
                output.append("  "_s, registration, '\n');

            auto* object = dynamicDowncast<JSObject>(record.cell);
            if (!object || !object->structure()->isBrandedStructure())
                continue;
            auto* branded = uncheckedDowncast<BrandedStructure>(object->structure());
            for (Symbol* symbol : privateSymbols) {
                if (branded->checkBrand(symbol))
                    output.append("  brand #"_s, m_ordinals.get(symbol), '\n');
            }
        }
        return output.toString();
    }

    JSGlobalObject& m_globalObject;
    VM& m_vm;
    Vector<JSCell*, 3> m_cellsWithoutEdges; // GlobalScope::WithoutEdges: the global scope's cells that are no root
    UncheckedKeyHashMap<JSCell*, unsigned> m_ordinals;
    Vector<Record> m_records; // by ordinal
    size_t m_nextToDescribe { 0 };
};

} // namespace HeapDescriptionInternal

String describeReachableHeap(JSGlobalObject& globalObject, std::span<const JSValue> roots)
{
    using namespace HeapDescriptionInternal;
    VM& vm = globalObject.vm();
    JSLockHolder locker(vm);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    // No collection interleaves with the walk, so the cells it holds by pointer stay alive, even while a host's reification
    // runs JavaScript inside it.
    DeferGCForAWhile deferGC(vm);

    HeapDescription description(globalObject);
    if (!roots.empty())
        return description.describe(roots, HeapDescription::GlobalScope::WithoutEdges);

    // The default roots: the global object, its global lexical environment and the module environment of each module
    // record the loader holds, in key order.
    Vector<JSValue> defaultRoots;
    defaultRoots.append(&globalObject);
    defaultRoots.append(globalObject.globalLexicalEnvironment());
    Vector<ModuleRegistryEntry*> entries;
    for (auto& entry : globalObject.moduleLoader()->moduleMap())
        entries.append(entry.value.get());
    std::ranges::sort(entries, [](ModuleRegistryEntry* a, ModuleRegistryEntry* b) {
        if (a->key() != b->key())
            return codePointCompareLessThan(a->key().string(), b->key().string());
        return a->moduleType() < b->moduleType();
    });
    for (auto* entry : entries) {
        AbstractModuleRecord* record = entry->record();
        if (JSModuleEnvironment* environment = record ? record->moduleEnvironmentMayBeNull() : nullptr)
            defaultRoots.append(environment);
    }
    return description.describe(defaultRoots.span(), HeapDescription::GlobalScope::Walked);
}

void writeReachableHeapDescription(JSGlobalObject& globalObject, const String& path)
{
    VM& vm = globalObject.vm();
    JSLockHolder locker(vm);
    // The run is over, and the shell has recorded the termination that ended it, if one did, by forbidding execution. Its
    // exception, or a request or trap still pending, would stop the walk at its first exception check, and putting the
    // exception back afterwards would need the request that the exit of the outermost VM entry scope withdrew. So the
    // description withdraws the termination for good; nothing the shell runs afterwards reads it.
    vm.cancelTermination();
    // A description is never empty, so a null one is a walk that threw.
    String description = describeReachableHeap(globalObject, { });
    if (description.isNull()) {
        SAFE_FPRINTF(stderr, "JITCache: %s: describing the heap threw\n", path.utf8());
        exit(1);
    }
    writeTestFile(path, description.utf8(), TestFileWrite::Replace);
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
