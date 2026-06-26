# 200 maintained Pharo packages for JIT correctness + speed testing

The kernel SUnit suite passes at Cog parity but under-covers real-world bytecode.
This is the **200-package expansion** of the [package-testing harness](jit-test-packages.md):
a curated set of maintained, test-bearing Pharo packages mined from the
[soogle](https://github.com/avwohl/soogle) index, to load and run on **both** the
custom JIT VM and stock Cog so any pass-rate divergence surfaces a JIT/VM bug.

The machine-readable manifest is **`scripts/pkg-jit-test/packages-200.tsv`**
(columns incl. the exact Metacello `load_expr`, the SUnit `test_prefix`, the test
path, and a headless-load risk). Run them with
**`scripts/pkg-jit-test/run-manifest.sh`** (best on the AWS build box — the
keep-alive lease now keeps that box up while a Claude drives the sweep).

## How the 200 were chosen (the funnel)

| stage | count | filter |
|-------|------:|--------|
| Pharo packages in soogle | 26,876 | `dialect=pharo` |
| maintained | 528 | pushed in the last year, non-fork, non-archived, GitHub |
| have a BaselineOf **and** tests | 318 | git-tree scan for `BaselineOf*` + `*Test*.class.st` |
| diverse ranked pool | 240 | per-org cap, image/VM forks dropped, ≥2 test classes |
| **final selection** | **200** | per-package verified load expr + P13 branch + risk class; dropped only P13-incompatible |

Every one of the 200 was checked against GitHub: its chosen branch resolves and
its `BaselineOf` file exists on that branch.

## Breakdown

- **Test path:** 173 headless **SUnit**, 27 **visual**
  (run via the [`pharo-headless-test`](https://github.com/avwohl/pharo-headless-test)
  fake-GUI prelude — Spec/Bloc/Roassal/Morphic/menu/graphics packages).
- **Headless-load risk:** 69 low / 84 med / 47 high
  (risk = chance it won't Metacello-load headless into P13; the harness records
  actual load success — risk is a hint, not a gate).
- **Pharo 13:** 83 yes / 117 maybe.
- **87 distinct GitHub orgs**, **6311 test classes** total.
- Coverage themes (JIT value): recursive-descent **parsing**, **serialization**
  /reflection, **numeric/float** (PolyMath/DataFrame/AI), **collection/hash**,
  large **object graphs** (Famix/Moose metamodels), **2D geometry** (graphics).

## Running the sweep

```sh
# on the box (16 vCPU), after a stock Pharo 13.1 + launcher is in place:
PHARO=/tmp/h3/pharo BASE_IMAGE=/tmp/h3/Pharo.image \
  CUSTOM_VM=$PWD/build-rel/test_load_image \
  scripts/pkg-jit-test/run-manifest.sh            # resumable; writes /tmp/pkg200/summary.tsv
```
Per package it: copies a clean image → stock-Cog Metacello load+save → runs the
suite on both VMs → records load status + the JIT-only failure count (candidate
regressions). Visual packages get the fake-GUI prelude automatically.

## The packages — headless SUnit (173)

| # | package | tests | risk | P13 | JIT value |
|---|---------|------:|------|-----|-----------|
| 1 | `tomooda/ViennaTalk` | 307 | high | maybe | parsing (PetitParser2), AST traversal, large object graphs,  |
| 2 | `hernanmd/BioSmalltalk` | 180 | med | maybe | parsing (SmaCC/PetitParser), string/collection, numeric/floa |
| 3 | `pillar-markup/pillar` | 168 | med | maybe | parsing, serialization, string processing, tree transformati |
| 4 | `pharo-contributions/XML-XMLParser` | 163 | low | yes | parsing, string/character processing, collection/hash, large |
| 5 | `pillar-markup/Microdown` | 160 | med | yes | parsing, serialization, string processing, tree transformati |
| 6 | `EiichiroIto/Avr-Pharo` | 157 | low | maybe | parsing, numeric/byte manipulation, collection |
| 7 | `moosetechnology/Famix` | 143 | med | yes | reflection, large object graphs, parsing/metamodel traversal |
| 8 | `pharo-rdbms/glorp` | 141 | low | yes | SQL string building/serialization, collection/hash, reflecti |
| 9 | `rko281/ReStoreForPharo` | 131 | med | maybe | object<->relational mapping, collections/hash, reflection, l |
| 10 | `punt-labs/anthropic-sdk-pharo` | 118 | med | maybe | JSON serialization/parsing, string handling, collections |
| 11 | `moosetechnology/FAST-JAVA` | 117 | med | maybe | parsing (SmaCC), AST/large-graph traversal, reflection, recu |
| 12 | `ReMobidyc/ReMobidyc` | 114 | med | maybe | parsing (PetitParser2), numeric/float simulation, collection |
| 13 | `koendehondt/hera-for-pharo` | 111 | med | maybe | acceptance/BDD test framework: parsing of feature text, refl |
| 14 | `pharo-vcs/iceberg` | 107 | high | maybe | VCS object graphs, tonel parsing/serialization |
| 15 | `ba-st/Willow` | 102 | med | maybe | HTML/AJAX string building and serialization, component rende |
| 16 | `pharo-contributions/mutalk` | 95 | low | yes | mutation testing: heavy bytecode/AST manipulation, method re |
| 17 | `OpenSmock/OpenSmock` | 82 | low | yes | reflection, collection/hash, small object graphs (UI model) |
| 18 | `Alamvic/druid` | 82 | high | maybe | JIT meta-compiler: IR/graph transforms, numeric/bytecode rea |
| 19 | `PolyMathOrg/PolyMath` | 80 | med | yes | numeric/float heavy (FFT, ODE, arbitrary-precision float, ma |
| 20 | `ba-st/Stargate` | 75 | med | yes | HTTP request parsing, JSON serialization, string/collection  |
| 21 | `FedeLoch/Ume` | 74 | high | maybe | mutation/fuzzing, recursion, reflection |
| 22 | `unicompute/GemStone-Pharo-Bridge` | 74 | high | maybe | serialization, object graphs |
| 23 | `moosetechnology/GitProjectHealth` | 71 | high | maybe | JSON parsing, mining/graph traversal, reflection |
| 24 | `pharo-spec/gtk-bindings` | 69 | high | maybe | FFI struct marshalling, reflection |
| 25 | `bouraqadi/PharoMisc` | 66 | low | maybe | concurrency/scheduling, equality/hash, reflection |
| 26 | `vonbecmann/earley-parser` | 64 | low | maybe | parsing, recursion, collection/hash |
| 27 | `pharo-cig/UnifiedFFI` | 57 | high | maybe | reflection, serialization |
| 28 | `astares/Pharo-OS-Windows` | 53 | high | yes | reflection |
| 29 | `Evref-BL/MCP` | 48 | med | yes | parsing, serialization, reflection |
| 30 | `terryc321/Pharo-Advent-Of-Code` | 47 | low | yes | parsing, numeric, collection/hash, recursion |
| 31 | `vonbecmann/fisher-parser` | 47 | low | maybe | parsing, recursion, collection/hash |
| 32 | `ba-st/Buoy` | 46 | low | yes | numeric/float math, collection/hash extensions, chronology,  |
| 33 | `pharo-cig/pharo-cig` | 39 | high | maybe | C-header parsing, reflection, large AST graphs — but needs n |
| 34 | `KendrickOrg/kendrick` | 37 | med | maybe | numeric/float ODE modeling via PolyMath, DataFrame collectio |
| 35 | `pharo-ai/graph-algorithms` | 35 | low | yes | graph traversal recursion, collection/hash, large object gra |
| 36 | `badetitou/Casino` | 32 | high | maybe | metamodel reflection, large model graphs, code migration par |
| 37 | `ApptiveGrid/Soil` | 31 | med | yes | serialization/deserialization, large object graphs, hashing, |
| 38 | `moosetechnology/Carrefour` | 31 | med | maybe | parsing, model graph traversal, reflection over FAST/Famix A |
| 39 | `vonbecmann/multi-valued-dictionary` | 30 | low | yes | collection/hash, equality, enumeration |
| 40 | `OpenSmock/Molecule` | 30 | med | yes | reflection, component contract dispatch, large object graphs |
| 41 | `kasperosterbye/PharoAIActions` | 29 | high | maybe | json serialization, string processing |
| 42 | `jecisc/Chanel` | 27 | low | yes | reflection, AST/code rewriting, collection iteration |
| 43 | `ba-st/Superluminal` | 26 | med | maybe | http request building, string/serialization parsing |
| 44 | `pharo-nosql/mongotalk` | 25 | med | maybe | BSON binary serialization, byte/collection manipulation, que |
| 45 | `feenkcom/PharoLink` | 25 | high | yes | json/msgpack serialization, socket I/O |
| 46 | `ba-st/Launchpad` | 24 | low | yes | CLI argument parsing, string handling, reflection over app r |
| 47 | `zeroflag/Teapot` | 24 | med | yes | HTTP routing, NeoJSON serialization, string/collection parsi |
| 48 | `ba-st/Hyperspace` | 23 | low | yes | URI/HTTP model parsing, string and collection manipulation |
| 49 | `moosetechnology/Fame` | 23 | low | yes | meta-model parsing, import/export serialization, reflection, |
| 50 | `JavierLarre/pharo-test-analysis` | 23 | med | maybe | AST/test reflection, method analysis, collection traversal |
| 51 | `omarabedelkader/HeuristicCompletion-History` | 22 | med | yes | completion history fetcher: collection/hash, string matching |
| 52 | `maxwills/SeekerDebugger` | 22 | high | maybe | time-travel debugger exercises reflection/large execution gr |
| 53 | `pharo-ai/metrics` | 20 | med | yes | machine-learning metrics: heavy float/numeric arithmetic and |
| 54 | `tinchodias/Ficus` | 19 | low | yes | recursion over tree graphs, immutable object construction, c |
| 55 | `Driolar/SoccerTheory-Pharo` | 19 | high | maybe | XML parsing, JSON serialization, graph algorithms, geometry/ |
| 56 | `pharo-llm/pharo-mcp` | 18 | med | maybe | JSON-RPC message parsing/serialization, string handling |
| 57 | `feenkcom/JSLink` | 18 | high | maybe | serialization/codegen of JS source strings |
| 58 | `pharo-ai/decision-tree-model` | 17 | low | yes | numeric/float computation (entropy/gini), collection/hash it |
| 59 | `pharo-ai/edit-distances` | 17 | low | yes | string comparison, numeric DP matrices, collection/array ind |
| 60 | `pharo-contributions/CSSParser` | 17 | low | yes | parsing, tokenization, string scanning, object-model constru |
| 61 | `vonbecmann/linked-list` | 17 | low | yes | collection traversal, pointer/object linking, iteration |
| 62 | `ba-st/Willow-SpinKit` | 17 | med | maybe | string building, HTML serialization, collection iteration |
| 63 | `shingarov/Pharo-ArchC` | 17 | high | maybe | PetitParser grammar parsing, formal-spec processing |
| 64 | `koendehondt/iris-for-pharo` | 16 | med | yes | JSON parsing/serialization, string handling, protocol dispat |
| 65 | `Pakarati/pharo-gsoc2025` | 16 | med | maybe | reflection, test-framework introspection, coverage instrumen |
| 66 | `fuhrmanator/FamixTypeScript` | 16 | high | maybe | reflection, large object graphs, metamodel generation |
| 67 | `pharo-ai/moose-linear-algebra` | 15 | low | maybe | numeric/float matrix arithmetic, sparse-vector collection/ha |
| 68 | `Evref-BL/Gitlab-Pharo-API` | 15 | low | maybe | JSON parsing/serialization, reflection (Mocketry mocking) |
| 69 | `ThalesGroup/GeoTools` | 15 | med | maybe | numeric/float-heavy geodesic and coordinate/kinematics math |
| 70 | `ba-st/Boardwalk` | 15 | med | maybe | string/JS generation, serialization, web request modelling |
| 71 | `lucretiomsp/Coypu` | 14 | high | maybe | numeric/audio DSP, OSC message serialization |
| 72 | `ba-st/Sagan` | 13 | med | maybe | serialization, persistence object graphs |
| 73 | `SmalltalkWeb/MyPrecious` | 13 | med | maybe | concurrency, recursion, collection |
| 74 | `ba-st/Stardust` | 13 | med | maybe | reflection, model description graphs |
| 75 | `mumez/RediStick` | 13 | high | maybe | socket protocol parsing, byte/string serialization, JSON |
| 76 | `feenkcom/JavaScriptGenerator` | 12 | low | maybe | code generation, string building, AST |
| 77 | `uca-argentina/2026-buscandoElPharo` | 12 | low | maybe | numeric, collection, game logic recursion |
| 78 | `chicoary/PharoWiki` | 12 | low | maybe | string, parsing |
| 79 | `rvillemeur/PharoMCP` | 12 | low | yes | JSON parsing/serialization, protocol message handling |
| 80 | `juliendelplanque/JRPC` | 12 | med | maybe | JSON parsing, serialization |
| 81 | `Evref-BL/Github-Pharo-API` | 12 | med | maybe | JSON parsing/serialization, reflection, object mapping |
| 82 | `moosetechnology/FamixTagging` | 12 | med | yes | large object graphs, reflection, metamodel traversal |
| 83 | `svenvc/NeoJSON` | 11 | low | yes | parsing, serialization, numeric/float, collection/hash, stre |
| 84 | `pharo-project/pharo-beacon` | 11 | low | yes | serialization, reflection, announcement/event dispatch |
| 85 | `pharo-ai/a-priori` | 11 | low | yes | collection/hash, frequent-itemset numeric, recursion |
| 86 | `j-brant/SmaCC` | 11 | med | maybe | parsing, recursion, large grammar tables, string/collection  |
| 87 | `ba-st/Kepler` | 11 | med | yes | reflection, dependency-injection lookups, namespace resoluti |
| 88 | `labordep/PharoGameye` | 11 | med | maybe | parsing, CSV serialization, collection |
| 89 | `moosetechnology/Esope` | 11 | med | maybe | parsing (PetitParser2), large object graphs, reflection |
| 90 | `newapplesho/google-cloud-smalltalk` | 11 | high | maybe | JSON parsing/serialization, JWT crypto/HMAC, numeric |
| 91 | `pharo-contributions/DeepTraverser` | 10 | low | yes | object-graph traversal, recursion, large graphs |
| 92 | `tesonep/ParametrizedTests` | 10 | low | maybe | reflection, collection, test-harness dispatch |
| 93 | `moosetechnology/FamixReplication` | 10 | med | yes | string hashing, collection/hash, duplication-detection over  |
| 94 | `alkalinan/pharo-openapi-generator` | 10 | med | maybe | JSON/OpenAPI-spec parsing and serialization, schema validati |
| 95 | `mumez/pharo-acp` | 10 | high | maybe | JSON-RPC parsing/serialization |
| 96 | `svenvc/ztimestamp` | 9 | low | yes | datetime parsing/formatting, numeric/integer arithmetic, str |
| 97 | `PolyMathOrg/vector-matrix` | 9 | low | yes | float/numeric matrix+vector arithmetic, collection indexing, |
| 98 | `svenvc/P3` | 9 | med | yes | binary protocol parsing, byte/string serialization |
| 99 | `jordanmontt/illimani-memory-profiler` | 9 | med | yes | allocation tracking, reflection/method-proxy wrapping, large |
| 100 | `pharo-rdbms/Pharo-SQLite3` | 9 | high | yes | FFI marshalling, byte/string serialization |
| 101 | `pharo-llm/pharo-huggingface` | 9 | high | maybe | JSON serialization/parsing, string handling |
| 102 | `pharo-ai/NgramModel` | 8 | low | yes | string/collection processing, hashing, dictionary/n-gram cou |
| 103 | `tinchodias/Sauco` | 8 | low | maybe | profiler tree/collection processing, reflection |
| 104 | `ba-st/Mole` | 8 | low | yes | graph algorithms (topological sort), collection/recursion he |
| 105 | `Evref-BL/Bitbucket-Pharo-API` | 8 | low | maybe | JSON parsing/serialization, string processing |
| 106 | `pharo-llm/pharo-acp` | 8 | low | yes | protocol/JSON message parsing and serialization |
| 107 | `Ducasse/Comix2` | 8 | low | maybe | XML parsing, string/collection processing |
| 108 | `pharo-contributions/OSSubprocess` | 8 | med | yes | FFI calls, stream/byte-array handling, process I/O |
| 109 | `punt-labs/postern` | 8 | med | yes | HTTP/string handling, reflection, image-driving logic |
| 110 | `pharo-ai/linear-models` | 7 | low | yes | linear/logistic regression: heavy float/matrix numeric math  |
| 111 | `tomooda/HoneyGinger` | 7 | med | maybe | SPH fluid simulation: heavy float/numeric and large array ma |
| 112 | `NathanFrund/blenny` | 7 | med | maybe | HTTP/JSON parsing, JWT crypto, string processing |
| 113 | `shnarazk/AoC-in-Pharo` | 7 | med | maybe | parsing (PetitParser2 grammars), collection/integer arithmet |
| 114 | `mumez/Ripple` | 7 | med | yes | JSON serialization, string/byte handling, websocket framing |
| 115 | `newapplesho/aws-sdk-smalltalk` | 6 | low | maybe | crypto/signing, string/byte hashing, JSON+XML serialization |
| 116 | `Ducasse/OSC` | 6 | low | yes | binary serialization, float/integer packing, byte-array hash |
| 117 | `Gabriel-Darbord/opentelemetry-pharo` | 6 | low | yes | reflection, MetaLink/MethodProxy bytecode interception, STON |
| 118 | `newapplesho/twilio-smalltalk` | 6 | low | maybe | JSON parsing/serialization, string/collection handling |
| 119 | `ApptiveGrid/FluxBase` | 6 | low | maybe | workflow-engine logic, collection/graph traversal |
| 120 | `NathanFrund/nexus` | 6 | low | maybe | JSON parsing, graph/collection traversal, hashing |
| 121 | `ApptiveGrid/PharoDEVS` | 6 | low | maybe | numeric simulation, recursion, collection/event-queue handli |
| 122 | `Evref-BL/PharoCompatibility` | 6 | low | yes | reflection, version-shim logic |
| 123 | `ronsaldo/pharo-newfile` | 6 | low | maybe | file/stream handling |
| 124 | `georghagn/TSF-Scheduler` | 6 | low | maybe | process scheduling, recursion, timing |
| 125 | `ErikOnBike/CodeParadise` | 6 | med | maybe | reflection, serialization, large object graphs, message disp |
| 126 | `Pharo-XP-Tools/DebuggingSpy` | 6 | med | maybe | reflection, debugger/context manipulation |
| 127 | `pharo-llm/pharo-infer` | 6 | med | maybe | parsing, reflection, collection/numeric inference logic |
| 128 | `svenvc/NeoCSV` | 5 | low | yes | parsing, stream/string processing, serialization |
| 129 | `pharo-contributions/MethodProxies` | 5 | low | yes | reflection, method activation/proxying, send interception |
| 130 | `pharo-containers/Containers-Trie` | 5 | low | yes | collection/hash, string traversal, recursion over tree nodes |
| 131 | `pharo-containers/Containers-Array2D` | 5 | low | yes | collection indexing, numeric, array bounds |
| 132 | `reugalabf/DatasetUtilities4Pharo` | 5 | low | yes | JSON/CSV parsing, string/serialization, timestamp arithmetic |
| 133 | `fouziray/EscapeAnalysisPharo` | 5 | low | maybe | AST/reflection, compiler analysis, call-graph traversal |
| 134 | `pharo-ai/hierarchical-clustering` | 5 | med | maybe | numeric/float math, matrix/distance computation, collection  |
| 135 | `moosetechnology/Famix-Cpp` | 5 | med | maybe | parsing (PetitParser2 grammar), large object graphs, reflect |
| 136 | `LNUitTutor/ShapesByPharo` | 5 | med | maybe | geometry/collection, numeric coordinate math |
| 137 | `pharo-contributions/SingularizePluralize` | 4 | low | yes | string parsing, collection/hash, regex matching |
| 138 | `pharo-ai/data-partitioners` | 4 | low | maybe | collection shuffle/partition, numeric proportions |
| 139 | `pharo-ai/stopwords` | 4 | low | yes | string/collection, hash set lookup |
| 140 | `RomaShmyhelskyi/PharoAthleteProject` | 4 | low | yes | basic collection/arithmetic logic |
| 141 | `georghagn/TSF-FileRotator` | 4 | low | yes | string/collection processing, file stream handling, date/age |
| 142 | `tomooda/Micromaid` | 4 | med | maybe | PetitParser2 grammar parsing, recursive descent, large graph |
| 143 | `ThalesGroup/PharoOWS` | 4 | med | maybe | XML parsing, JSON serialization, string processing |
| 144 | `moosetechnology/Famix-ExecutionFlow` | 4 | med | maybe | metamodel reflection, large object graphs, code generation |
| 145 | `pharo-ai/gaussian-mixture-model` | 4 | med | yes | numeric/float-heavy iterative fitting, matrix/collection mat |
| 146 | `StefanKrecher/PharoCodex` | 4 | med | maybe | string/JSON parsing and serialization |
| 147 | `Evref-BL/BitbucketCloud-Pharo-Api` | 4 | med | yes | JSON serialization/parsing, mocking reflection |
| 148 | `omarabedelkader/pharo-lexicon` | 4 | med | maybe | reflection, AST/code parsing and analysis |
| 149 | `georghagn/TSF-NexIO` | 4 | med | yes | JSON parsing/serialization, string/byte stream handling |
| 150 | `Ducasse/Chrysal` | 3 | low | yes | config-file parsing, string/collection processing, reflectio |
| 151 | `NathanFrund/Conduit` | 3 | low | maybe | string parsing/templating, collections |
| 152 | `pharo-containers/Containers-Stack` | 3 | low | yes | collection/array indexing, LIFO ops, iteration |
| 153 | `georghagn/TSF-Logger` | 3 | low | yes | string formatting/serialization, collection iteration, polym |
| 154 | `noha/JSONWebToken` | 3 | med | yes | base64/hash/HMAC crypto, JSON serialization, string processi |
| 155 | `mumez/PharoSmalltalkInteropServer` | 3 | med | yes | HTTP request routing, JSON parsing/serialization |
| 156 | `moosetechnology/FAST` | 3 | med | yes | AST model construction/visitor traversal, reflection, large  |
| 157 | `NathanFrund/Datastar-Pharo-SDK` | 3 | med | maybe | string/HTTP parsing, serialization, collections |
| 158 | `rko281/Porpoise` | 3 | med | maybe | reflection, FFI marshalling, string/collection compat |
| 159 | `omarabedelkader/HeuristicCompletion-Analyser` | 3 | med | maybe | reflection, package/symbol analysis, collections/hashing |
| 160 | `omarabedelkader/HeuristicCompletion-Benchmarks` | 3 | med | maybe | string/collection prefix matching |
| 161 | `pharo-llm/pharo-dataset` | 3 | med | maybe | JSON serialization, string/collection processing |
| 162 | `Pharo-XP-Tools/Sindarin-DAP` | 3 | med | maybe | reflection, debugger/Sindarin stack walking |
| 163 | `jordanmontt/pharo-instrumentation` | 3 | med | yes | reflection/method-proxy wrapping, bytecode recompilation, se |
| 164 | `akgrant43/GeoSphere` | 2 | low | yes | float/numeric math (distance computation), coordinate string |
| 165 | `ApptiveGrid/Canopy` | 2 | low | yes | collection/hash, reflection (global value management) |
| 166 | `omarabedelkader/HeuristicCompletion-Prefix` | 2 | low | yes | parsing, reflection, string prefix scanning |
| 167 | `svenvc/NeoConsole` | 2 | med | yes | string/stream parsing, REPL command dispatch |
| 168 | `Evref-BL/Pharo-LLMAPI` | 2 | med | yes | JSON serialization/parsing, string handling, mocked HTTP req |
| 169 | `seandenigris/Pharo-Enhancements` | 2 | med | maybe | core-extension utility methods, collection/string helpers |
| 170 | `pharo-llm/pharo-agent` | 2 | med | maybe | parsing, serialization (JSON/agent message handling) |
| 171 | `KentBeck/SmalltalkGenie` | 2 | med | maybe | JSON serialization/parsing, string handling |
| 172 | `Evref-BL/AI4Pharo` | 2 | med | maybe | parsing, serialization (LLM message/JSON) |
| 173 | `reugalabf/OpenWeatherAPI4Pharo` | 2 | med | yes | JSON/CSV parsing, serialization, float/numeric |

## The packages — visual / pharo-headless-test (27)

| # | package | tests | risk | P13 | JIT value |
|---|---------|------:|------|-----|-----------|
| 1 | `pharo-spec/Spec` | 243 | high | yes | reflection, collection/hash, large graphs |
| 2 | `pharo-spec/NewTools` | 195 | high | yes | reflection, collection, large graphs |
| 3 | `pharo-graphics/Bloc` | 138 | high | maybe | 2D geometry/float math, layout traversal |
| 4 | `OpenSmock/Pyramid` | 118 | high | maybe | UI model serialization, geometry/float |
| 5 | `moosetechnology/MooseIDE` | 102 | high | maybe | large model graphs, reflection, visualization layout math |
| 6 | `pharo-llm/chatpharo` | 91 | high | maybe | JSON serialization, agent/tooling dispatch, string handling |
| 7 | `pharo-graphics/Roassal` | 87 | high | maybe | geometry/float math, layout algorithms, large graphs |
| 8 | `GemTalk/JadeiteForPharo` | 65 | high | yes | reflection, large graphs |
| 9 | `ThalesGroup/GeoView` | 61 | high | maybe | numeric/float, large graphs |
| 10 | `pharo-graphics/Alexandrie` | 40 | high | maybe | FFI native canvas calls, float geometry — but unusable witho |
| 11 | `mumez/pharo-agentic-browser` | 31 | high | maybe | JSON/protocol serialization, agent session graphs |
| 12 | `cormas/cormas` | 24 | high | maybe | agent-based simulation numeric/collection loops |
| 13 | `OpenPonk/petrinets` | 23 | high | maybe | petri-net token-flow simulation |
| 14 | `pharo-llm/pharo-llmstudio` | 23 | high | maybe | NeoJSON request/response serialization |
| 15 | `pavel-krivanek/Turbo` | 22 | med | maybe | layout/animation/canvas numeric and collection math |
| 16 | `OpenSmock/Bloc-Serialization` | 21 | high | maybe | STON/Stash serialization of element graphs (good) but graphi |
| 17 | `exercism/pharo-smalltalk-test-runner` | 20 | high | maybe | SUnit harness logic, minimal |
| 18 | `Ducasse/Myg` | 19 | high | maybe | game logic (Array2D, board solving) but UI deps block headle |
| 19 | `Olesia32/pharo-spec-components` | 19 | high | maybe | UI widget construction, minimal core compute |
| 20 | `pharo-graphics/pharo-sdl-experiments` | 17 | high | maybe | FFI marshalling, R-tree/BooleanArray collection ops |
| 21 | `pharo-graphics/PharoSDL3` | 16 | high | maybe | FFI marshalling, struct access |
| 22 | `olekscode/MicroUML` | 10 | med | yes | parsing, AST building, code generation (string-heavy) |
| 23 | `pharo-graphics/Spec-Toplo` | 10 | high | yes | UI graph, collection |
| 24 | `OpenPonk/fsm-editor` | 10 | high | maybe | modeling graph, serialization |
| 25 | `neerja-1984/PAM-GSoC-25-Project` | 9 | high | maybe | audio DSP numeric/float, but unloadable |
| 26 | `OpenSmock/Iconography` | 5 | med | maybe | SVG/XML parsing, image/byte processing |
| 27 | `neAnasteisha/Pharotales_Books` | 3 | med | maybe | none |
