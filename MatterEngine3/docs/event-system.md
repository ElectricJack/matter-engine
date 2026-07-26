# Event System — Unified Notifications, Observable Models, and the Command Layer

> Design spec for one standardized way for engine, editor, and (eventually) gameplay systems to announce things and react to them without holding pointers to each other. Today every subsystem reinvents its own signaling — a mutex+deque event queue, a virtual observer, error-sink interfaces, polled booleans, and flecs observers all coexist with no shared idiom and no way to see what fired. This spec defines a three-part taxonomy (notifications / commands / strategy callbacks), an ergonomic typed pub-sub core with per-event immediate-vs-queued delivery, observable models for data-binding, deterministic listener ordering with first-class observability, and a command layer shaped to grow undo/redo. Usability and standardization are designed first; implementation is phased second and deliberately light — this document settles the design.

- **Target:** new public `MatterEngine3/include/matter/event/` interfaces plus `MatterEngine3/src/event/` implementations (hub, transport, observable models, command layer) + incremental migration of existing signaling sites in `matter_engine.cpp`, `async_bake.h`, MatterEditor, and the ECS bridge
- **Baseline:** `34213f65` (feature/bake-lab)
- **Status:** Spec — design settled after adversarial review (topology, the five original questions, and the seven implementation-contract corrections resolved in §II.3–II.4); Part II milestones ready to sequence; implementation not started
- **Relation:** builds on the observability tooling from [bake-lab.md](bake-lab.md) (BakeTrace spans, Timeline flamegraph) for the event inspector; respects [part-workbench.md](part-workbench.md)'s `BakeObserver` (W3) as a shipping seam that later re-expresses on this system.

---

## Part I — Design Spec

### I.1 Problem statement

The engine has at least six unrelated signaling mechanisms, and producers are directly coupled to their consumers:

1. **The bake `Event` queue** — `include/matter/events.h:11-42`, a mutex-guarded `std::deque<Event>` capped at 4096 with *silent* drop-oldest (`matter_engine.cpp:338-343`, `676-678`), fed by ~40 `emit_event` call sites on the bake worker and GL thread, drained by the app thread via `WorldSession::poll_event` (`matter_engine.cpp:4638`). The `EventType` enum is becoming a dumping ground: `RefineTileDone` was bolted on for streaming progress (`events.h:17`), and the `Event` struct grows append-only fields (`phase`, `tile_tx/tz`) that most events ignore.
2. **`BakeObserver`** — `include/matter/bake_observer.h`, a virtual-callback seam added for the Part Workbench's live bake watch (W3). Fires on the bake worker *or* GL thread, null-checked at every site, Lab-only. A second, incompatible idiom for the same job the event queue does.
3. **Hand-rolled queues** — the mutex+deque+poll shape appears four times: the `Event` queue above, `GpuJobQueue` and `CommandQueue` (`src/async_bake.h:28-73`), and `FileWatcher::poll` (`src/file_watcher.h:24-33`). `GpuJobQueue` is the most mature cross-thread transport (worker→GL posting, time-budgeted `pump`, `run_blocking` latch, cancel tokens, shutdown draining) — and none of that maturity is shared with the other three.
4. **Polled flags** — the crudest idiom, used for viewer-internal requests: `ViewerStats::reload_requested` / `world_switch_requested` (`MatterEditor/src/ui.h:73,102`), the `WorkbenchHandoff` struct (`ui.h:51`), `focus_workbench_tab_` (`bake_lab.h:68`). "Set a bool, someone polls it next frame" — no fan-out, no trace, ownership by comment convention (`// panel sets; main clears after handling`).
5. **flecs observers** — the ECS already uses a real event system for structural changes: `world.observer<T>().event(flecs::OnAdd/OnSet/OnRemove)` in `physics_systems.cpp:20-57`, `transform_system.cpp:400-422`, `streaming_systems.cpp:159-168`. Synchronous, ECS-tick-thread, entity-scoped — good at what it does, unusable for cross-thread engine plumbing or non-entity state.
6. **Notification sink interfaces** — `BridgeErrorSink` (`src/ecs/dynamic_scene_bridge.h:31-33`, two `std::function`s) and live-edit `ErrorSink` (`src/live_edit_interfaces.h:58-62`, a virtual). Each is a bespoke one-consumer contract for what is semantically a fan-out notification ("an error happened; whoever cares should hear about it").

And one half-built foundation: `PhysicsContext::capture_events` (`src/ecs/physics_context.cpp:1074`) collects Box3D contact/sensor/body events into a `PhysicsEvents` snapshot every step (`include/matter/physics.h:107`), but **nothing consumes it** except `tests/physics_tests.cpp`. The gameplay-event pipeline exists up to the point where someone would need to subscribe — and there is no way to subscribe.

There is no central dispatcher anywhere (no EventBus/Signal/dispatch construct exists in the codebase). Consequences:

- **Coupling:** producers hold pointers to consumers (`BakeObserver*` threaded down the bake path; sinks passed by reference through call chains; viewer panels writing directly into `ViewerStats`). Adding a listener means touching the producer.
- **No standard idiom:** every new feature invents signaling again — W3 invented `BakeObserver`, the asset browser invented `WorkbenchHandoff`, streaming bolted `RefineTileDone` onto the bake enum.
- **Zero observability:** when "game start" (or today, "bake finished") triggers a cascade, nothing records what fired, who received it, or in what order. The user reports this exact class of bug as historically painful to debug.
- **No data-binding:** a value shown in three UI places (a selected entity's transform in the gizmo, properties panel, and scene tree) has no change-notification story at all; the UI re-reads everything every frame or goes stale.

### I.2 Goals and non-goals

**Goals:**

- **One idiom.** Declaring an event type, subscribing (RAII), emitting, and choosing delivery is the same five lines everywhere — engine internals, editor UI, and later gameplay scripts. Usability is the first design criterion, not a wrapper added later.
- **Decoupling.** Producers emit; they hold no subscriber pointers. Subscribers register against the hub; they never touch the producer. Adding the Nth listener touches only the listener.
- **Fold in all notifications** regardless of thread, with per-event immediate (same-thread subscriber-list walk, no queue cost) vs. queued (cross-thread, drained by the owner) delivery. Same-thread notifications belong in the system too: the payoff is fan-out, decoupling, and one place to trace flow.
- **One transport primitive.** The four hand-rolled mutex+deque queues consolidate onto a single well-tested channel type with bounded-capacity policies, budgeted pump, and blocking latch — `GpuJobQueue`'s maturity, shared.
- **Deterministic, observable ordering.** Explicit phases + priorities + stable tie-break; an introspectable registry (who subscribes to what, in what order); an event trace feeding an in-editor inspector built on the Bake Lab's Timeline tooling. "You can always see what fired, who received it, and in what order" is a core requirement.
- **Observable models** as a first-class layer: bindable properties/models with dirty-coalescing change notification, for values mirrored across UI.
- **A command layer** on top: named, parameterized, deliver-once actions, shaped so undo/redo can be added without redesign.
- **flecs stays the entity-event system.** Structural ECS observers are untouched; entity-shaped gameplay events (physics contacts) are delivered through flecs; the in-house core covers everything that isn't entity-shaped.

**Non-goals:**

- **No third-party event library** (eventpp, entt::dispatcher, Boost.Signals2). Settled with the user: the engine already vendors flecs (a capable entity-event system) and already owns four queue implementations to consolidate; a generic signal library would be a redundant dependency solving the easy 20% (the subscriber-list walk) while leaving the hard 80% (threading contracts, ordering determinism, observability, migration) untouched. Mentioned only to record why they were rejected.
- **No conversion of strategy/policy callbacks** (§I.3.3) — they are excluded by semantics, permanently.
- **No full undo/redo implementation** in this spec — the command layer is *shaped* for it (§I.10); the undo stack itself is follow-up work.
- **No lock-free/wait-free transport heroics.** The mutex+deque shape is proven at current scale (a few thousand events per bake); the win is consolidation and semantics, not nanoseconds. Revisit only with profiler evidence.
- **No network/replication story.** Single-process only.
- **No immediate rewrite of the wire-level `WorldSession::poll_event` API** — external consumers keep working through a compat shim until migration completes (§I.11).

### I.3 The taxonomy — semantics, not threading

Every signaling mechanism in the engine falls into exactly one of three buckets. The dividing line is **what the interaction means**, never which thread it happens on. This taxonomy was settled with the user and is the load-bearing decision of this spec: mechanisms migrate (or don't) based on which bucket they're in.

#### 1. Notifications — "X happened" (these are events)

Fan-out to N observers, zero or more of whom may exist; **no return value**; the emitter proceeds identically whether anyone listened. All notifications fold into the new system regardless of thread — the bake progress events, `BakeObserver`'s rung callbacks, the error sinks, viewer state changes, and (eventually) the orphaned physics events. Per-event delivery is immediate or queued (§I.6), but that is a *transport* choice, not a semantic one.

#### 2. Commands/requests — "do X" (deliver-once)

Exactly one handler; the request is *consumed*, not observed; it may be **superseded or cancelled** before handling (today's `CommandQueue`: a new `BakeAll`/`Reload` cancels the in-flight command's token and clears pending — `async_bake.h:55-58`). Commands share the queue **transport** with notifications but not the fan-out semantics. The command layer (§I.10) is built on top of this system and is the foundation for undo/redo. The viewer's polled flags (`reload_requested`, `world_switch_requested`, `WorkbenchHandoff`) are commands in disguise — deliver-once requests currently smuggled through shared state.

#### 3. Strategy/policy callbacks — "answer X" / "you are the authority" (not events)

Return a value, or name a single decision-maker: `scene::PartResolver` (`std::function<bool(module, out_hash)>`, `scene_registry.h:65`), `PendingEvictionBatch::Endpoint` (`sector_streaming_coordinator.h:97`), Box3D ray/overlap query callbacks. A fan-out bus cannot resolve "which subscriber's return value wins" — these have exactly-one-authority semantics and stay as injected interfaces. **Excluded for semantic reasons, not threading.** The litmus test for future code: *does the emitter's behavior depend on the response?* If yes, it is a strategy interface; if no, it is a notification.

### I.4 Approach comparison

#### A — Extend flecs' event API to everything

flecs (vendored, `third_party/flecs`) has a first-class observer/emit API already used for structural events, with ordering, filtering, and an ecosystem of introspection tooling.

- **Pro:** zero new core code; one event system on paper; entity events are its home turf.
- **Con — disqualifying:** flecs events are **ECS-world-scoped and effectively single-threaded** — emission happens inside the world, observers run on the ECS tick thread (or defer to the merge). Engine plumbing is exactly the opposite shape: bake-worker → app-thread progress, GL-thread publish callbacks, cross-thread error reporting. Wedging those in means every emitter must marshal into the ECS world first, inheriting its tick cadence and staging rules.
- **Con:** most engine/editor events are not entity-shaped. "Bake finished," "file changed," "reload requested," and a bound properties-panel float have no entity; inventing carrier entities for them is ceremony that obscures rather than standardizes.
- **Con:** the editor's observable models (§I.9) need prime-on-subscribe and coalescing semantics flecs does not provide for non-component state.

#### B — Standalone in-house event system for everything (including entity events)

- **Pro:** one hub, one trace, one inspector covering literally everything.
- **Con:** duplicates what flecs already does well. Structural observers (`OnAdd/OnSet/OnRemove`) are deeply integrated with archetype storage — reimplementing entity filtering, query-scoped observation, and staging outside flecs is a large, permanent maintenance tax, and the existing `physics_systems.cpp`/`transform_system.cpp` observers would need migration for zero behavioral gain.
- **Con:** gameplay events want entity context (source entity, component filters, "observer on this entity only") — rebuilding that outside the ECS reinvents flecs badly.

#### C — Hybrid: flecs for entity-shaped events, in-house core for everything else **(chosen)**

flecs remains the event system **inside the ECS world**: structural observers stay exactly as they are, and gameplay events on entities (physics contacts, sensor triggers, entity lifecycle) are delivered as flecs entity events on the tick thread. The new in-house core (`evt::Hub`) covers everything that crosses threads or isn't entity-shaped: engine plumbing (bake/stream/live-edit progress, errors), editor UI signaling, observable models, and the command layer. One bridge rule keeps the halves observable as a whole: flecs-side emissions can be mirrored into the hub's **trace** (not re-dispatched) so the event inspector shows one unified timeline.

- **Pro:** each half does what it is natively good at; no marshaling ceremony for either.
- **Pro:** zero migration risk for the working flecs observers; the orphaned `PhysicsEvents` finally gets a delivery mechanism that matches its shape (entities, tick thread).
- **Pro:** the in-house core is small — precisely because it does *not* attempt entity filtering. It is a typed subscriber registry + the consolidated transport queue + trace hooks.
- **Con:** two subscription APIs exist. Mitigated by making them rhyme (same naming convention for event types, same trace format) and by a clear routing rule: **entity-scoped and tick-synchronous → flecs; everything else → hub.** The spec's migration map (§I.11) applies the rule to every existing site so there is no judgment call left.

> **Decision:** Approach **C**. flecs for entity/gameplay-shaped events; in-house `evt` core for engine plumbing, editor/UI, observable models, and commands; unified tracing across both.

### I.5 The core API — usability and standardization

This section is the heart of the spec: the idiom every subsystem will use. Design rules first, then the sketches.

**Rules:**

1. **An event is a plain struct** with a compile-time name. No inheritance, no registration ceremony beyond the descriptor line. Payloads are values (copyable); the queued path copies them, so no lifetime questions.
2. **Every subscription has a name.** Mandatory, not optional — the name is the ordering tie-break (§I.7), the registry identity, and what the trace shows. Names are unique per event type in **every build**: `try_subscribe` returns `DuplicateName`, while the ergonomic `must_subscribe` form invokes the process registration-error handler and never inserts the duplicate. Anonymous lambdas with no accountable name are exactly how ordering bugs became undebuggable historically.
3. **Subscriptions are RAII, with explicit quiescence for cross-thread owners.** `evt::Subscription` owns a shared callback control block and its destructor performs a thread-safe logical unsubscribe: no new dispatch may begin after it returns, and queued deliveries resolve the live registry at pump time rather than retaining a raw callback. A callback already executing on another thread may finish, so an owner whose callback captures `this` calls `SubscriptionSet::unsubscribe_all_and_wait()` at the start of its destructor, before invalidating captured state. Same-lane owners may rely on ordinary RAII once that lane is not pumping. `Hub::close()` invalidates every handle, cancels queued delivery, and waits for in-flight callbacks after emitters have stopped (§I.6). This is the actual no-dangling-listener contract; RAII alone is not claimed to be a cross-thread barrier.
4. **Delivery mode is declared by the subscriber**, per subscription: `immediate` (run on the emitter's thread, inline) or a named **lane** (queued; drained by the thread that owns the lane). The emitter never knows or cares — it emits once; the hub walks immediate subscribers inline and enqueues one copy per subscribed lane.
5. **Emitting is one call.** No builder chains, no channel lookups at the call site.

#### Declaring an event type

```cpp
// include/matter/events/bake_events.h  (event types live in small, per-domain headers)
namespace matter::events {

struct PartBaked {
    MT_EVENT_NAME("bake.part_done");    // compile-time name: trace key, script name, registry id
    std::string module;
    int done = 0, total = 0;
};

struct BakeFailed {
    MT_EVENT_NAME("bake.error");
    std::string module, message;
    BakeErrorCode code = BakeErrorCode::None;
};

}  // namespace matter::events
```

`MT_EVENT_NAME` expands to a `static constexpr` name + a stable type id. Names are dot-namespaced (`bake.*`, `stream.*`, `edit.*`, `viewer.*`, `error.*`, `phys.*`) — the namespace is what the inspector filters on and what scripts will subscribe by. This directly replaces the `EventType` enum dumping ground: `RefineTileDone` stops being a bake event and becomes `stream.refine_tile` with its own honest payload (no more `-1`-when-unused `tile_tx` fields riding on every bake event — `events.h:40-41`).

#### Subscribing (RAII, named, mode-declared)

```cpp
class BakeHud {
    evt::SubscriptionSet subs_;     // destructor unsubscribes everything
public:
    explicit BakeHud(evt::Hub& hub) {
        // Queued: bake worker emits, app lane drains next pump — today's poll_event shape.
        subs_ += hub.must_subscribe<events::PartBaked>(
            "hud.bake_progress", evt::lane::app,
            [this](const events::PartBaked& e) { progress_ = float(e.done) / e.total; });

        // Immediate: same-thread fan-out, no queue cost. Runs on whatever thread emits.
        subs_ += hub.must_subscribe<events::BakeFailed>(
            "hud.error_flash", evt::immediate,
            [this](const events::BakeFailed& e) { note_error(e.message); });
    }
};
```

Ordering controls (phase/priority, §I.7) are optional arguments with sensible defaults — the simple case stays two lines. Runtime-extensible systems use `try_subscribe` and handle `RegistrationError`; statically wired engine/editor systems use `must_subscribe`, whose all-build fail-fast behavior treats a duplicate name as a configuration bug rather than accepting nondeterminism.

#### Emitting

```cpp
hub.emit(events::PartBaked{module_name, done, total});
```

Thread-safe from any thread. Immediate subscribers run inline on this thread (contract: §I.6); each lane with subscribers gets one copied enqueue. If nobody subscribed, cost is one hash lookup — cheap enough to emit unconditionally, which is what makes "emit generously, subscribe as needed" viable as the standard idiom.

#### Draining a lane

```cpp
// main.cpp frame loop — replaces the poll_event / pump_gpu_jobs / flag-check trio:
hub.pump(evt::lane::app, /*ms_budget=*/2.0);
```

`pump` mirrors `GpuJobQueue::pump`'s proven contract (`async_bake.h:33-35`): runs whole deliveries until the budget elapses or the lane empties, always makes progress when work is pending, returns the count delivered.

#### Tearing down a cross-thread subscriber

```cpp
BakeHud::~BakeHud() {
    // Required because immediate handlers may be running on a bake worker.
    // Illegal from inside one of this set's own callbacks (fail-fast in all builds).
    subs_.unsubscribe_all_and_wait();
}
```

Logical unsubscribe (`Subscription::reset`, also called by the destructor) is non-blocking and reentrancy-safe; quiescent unsubscribe is the explicit lifetime barrier for callbacks that capture owner state. The hub tracks an in-flight count in each shared callback control block. `unsubscribe_all_and_wait()` first marks every block inactive, then waits until their counts reach zero. It must not be called while holding a lock that a callback may need, while executing inside the affected lane's pump, or from one of the affected callbacks. `Hub::close()` uses the same two-phase protocol, but its precondition is stronger: all external emitters have been stopped or joined first.

#### Gameplay scripts (QuickJS, future)

The compile-time string names are the script-facing contract. A future binding is mechanical:

```js
engine.events.on("bake.part_done", (e) => { hud.set(e.done / e.total); });
```

Script subscriptions are always queued onto the script-tick lane (scripts never run on the emitter's thread), and payloads cross via a per-type JSON serializer registered alongside the descriptor — optional in C++, required only for types exposed to scripts. Nothing in the core needs to change when this lands; it is subscriber #N+1. Entity-shaped gameplay events reach scripts through the flecs side's own binding when Phase 6 (gameplay scripting) arrives.

#### The consolidated transport primitive

Beneath the hub sits the one queue type that replaces the four hand-rolled ones:

```cpp
// evt::Channel<T> — the single mutex+deque primitive, extracted and hardened.
template <class T> class Channel {
public:
    enum class PushResult { Queued, Coalesced, RejectedFull, ShutDown };
    enum class WaitResult { Item, Timeout, ShutDown };
    enum class OnFull { RejectNewest, DropOldest, CoalesceNewest, BlockProducer };

    struct Policy {
        size_t capacity;                             // 0 = unbounded
        OnFull default_on_full;
    };
    struct PushOptions {
        OnFull on_full;                              // explicit per push
        std::optional<uint64_t> coalesce_key;        // required for CoalesceNewest
    };

    explicit Channel(Policy policy);       // policy is mandatory: lossiness is never accidental
    PushResult push(T item);               // uses mandatory channel default
    PushResult push(T item, PushOptions);  // descriptor/policy override; any thread
    bool       try_pop(T& out);             // single consumer, non-blocking
    WaitResult wait_pop(T& out);            // single consumer, blocking
    WaitResult wait_pop_for(T& out, std::chrono::milliseconds timeout);
    bool       run_blocking(T item, ...);    // push + completion latch (GpuJobQueue shape)
    int        pump(double ms_budget, const std::function<void(T&)>& fn);
    int        pump_one(const std::function<void(T&)>& fn); // owner thread, at most one
    void       shut_down();                  // reject future pushes, fail pending, wake all waiters
    uint64_t   dropped() const;              // monotonic intentional-loss counter
    uint64_t   rejected() const;             // monotonic caller-visible rejection counter
};
```

Policies are mandatory at channel construction, not inherited from a dangerous library default. Hub notification lanes choose bounded `DropOldest` as their lane default, then pass descriptor-specific `PushOptions` for exceptions. A state-shaped descriptor may request `CoalesceNewest` with a hub-composed `(event type, application key)`; if no matching queued envelope exists, the push is rejected rather than replacing an unrelated event. Command channels are **non-dropping by contract**: the existing bake `CommandQueue` remains unbounded (`capacity = 0`) to preserve behavior, while externally fed command channels use bounded `RejectNewest` and complete the rejected command ticket with `QueueFull`. Once accepted, a command reaches exactly one handler or an explicit terminal cancellation. Commands never use `DropOldest` or `CoalesceNewest`; explicit supersession is a distinct command policy that completes displaced tickets as `Superseded`.

`GpuJobQueue` and `CommandQueue` become thin wrappers over `Channel` preserving their exact public APIs and semantics — `pump`/`run_blocking` for the former, `wait_pop`/`wait_pop_for` plus cancel-token supersession for the latter. `BlockProducer` exists only for the legacy blocking-latch path and rejects an attempt from the channel's owner thread, which would deadlock. These queues are *transport consumers*, not hub events (GPU jobs are work submission; commands are §I.10's layer). `FileWatcher` keeps its OS-facing `poll` interface but `LiveEditSession` republishes debounced results as `edit.file_changed` notifications, so tools can observe live-edit activity without touching the watcher.

### I.6 Threading contract per delivery mode

Threads in play: **app/UI thread** (equals the GL thread in this codebase but treated as a distinct contract, per the existing `assert_gl_thread` discipline), **bake worker**, **ECS tick** (flecs side), external FIFO reader (`MATTER_CMD_FIFO`), and Lab job threads. The contract is explicit per mode and enforced with the debug-assert pattern already proven by `matter_async::register_gl_thread`/`assert_gl_thread` (`async_bake.h:78-79`).

**Immediate delivery:**

- Runs synchronously on the **emitter's thread**, inside `emit()`. The subscriber inherits every constraint of that thread — the `BakeObserver` header's rules generalize verbatim (`bake_observer.h:13-28`): treat the thread as unknown unless the event type documents an affinity; never touch GL/ImGui unless the event is documented app-thread-only.
- Subscribers must be **fast and non-blocking** — an immediate subscriber that takes a lock held by the emitter's caller deadlocks by construction.
- **Reentrancy:** subscribing or logically unsubscribing during dispatch is legal. The current dispatch walks shared callback control blocks and checks their active bit immediately before entry; a handler removed before its turn is skipped, while one already executing is allowed to finish. Quiescent `unsubscribe_all_and_wait` from the affected callback or lane pump is illegal and fails fast in every build. Emitting from inside a handler is legal but depth-capped (all-build fail-fast at depth 8) — cycles are a bug, and the trace records nesting so they are visible, not mysterious (§I.8).
- Subscribers must not re-enter the emitting subsystem's mutable API from inside the handler (the generalization of "don't call GL from `on_rung_ready`"). Event-type docs name the emitter's off-limits surface where it isn't obvious.

**Queued delivery:**

- Payload is **copied at emit**; the subscriber runs when the lane's owner thread pumps. No shared-lifetime questions — this is why payloads are plain values.
- Each lane has exactly one owning thread, registered at startup (`hub.claim_lane(lane::app)`); pumping from any other thread is a debug abort.
- **Bounded by default** (4096, matching today), but **never silently**: every notification lane counts drops, the hub writes an out-of-band drop record directly to its trace/registry counters, and the inspector surfaces it in red. It does **not** enqueue an `evt.dropped` event onto the overflowing channel, avoiding recursive drop reporting. An optional throttled `evt.dropped` notification is emitted only onto a distinct diagnostics lane after capacity is available. `CoalesceNewest` is descriptor-opt-in and keyed by `(event type, application key)`; it cannot replace unrelated envelopes. Commands use the non-dropping policies in §I.5 and are never represented by a drop counter.
- Ordering *within one lane from one emitter* is FIFO; ordering across lanes or emitters is not promised — code needing a cross-thread ordering guarantee must use one lane.

**Subscription mutation and shutdown:** registration and logical unsubscribe are thread-safe. A queued envelope stores the copied event plus its lane and sequence, not raw callback pointers; at pump time the hub snapshots that event type's currently active lane subscribers, increments each callback control block's in-flight count before entry, and decrements it on exit. Thus unsubscribe before pump suppresses delivery, and quiescent unsubscribe has a finite condition to wait on. Session destruction follows a fixed order: stop accepting work, cancel commands, shut down GPU waits, join the bake worker, call `session_hub.close()` to cancel queued events and wait for immediate callbacks, then destroy ECS/GL state. Hub handles retain a weak hub-state reference, so destroying a handle after its hub is safe and is a no-op.

**Per-event-site audit:** delivery mode is not a global default — each migrated site (§I.11) picks its mode deliberately, and the choice is recorded in the event-type header comment. The dangerous migration direction is sync→queued (a consumer that assumed it ran before the emitter's next line now runs a frame later); §I.12 treats this as a first-class risk with a per-site checklist.

### I.7 Deterministic listener ordering

The historical pain: multiple listeners on one event ("game start") needing a specific order, with the actual order an accident of registration sequence — unreproducible across runs and undebuggable. The design kills accidental order at the root:

**Ordering key = (phase, priority, subscription name).** Comparison is lexicographic over the triple; the hub keeps each event type's subscriber list sorted by it.

- **Phases** are a small, fixed, named enum — `evt::phase::First, Early, Default, Late, Last` — not user-extensible strings. Phases express *intent* ("I must see this before normal handlers") and are what code review checks. Five is deliberate: an open-ended phase graph (à la dependency edges between named systems) was considered and rejected — dependency-graph ordering is flexible but makes the effective order emergent again, which is precisely the disease. If two subscribers genuinely need a strict relationship beyond phases, that is a smell that they should be one subscriber calling two functions.
- **Priority** is an `int` within a phase, default 0, for the rare fine-grained case.
- **Name** is the total-order tie-break. Because names are mandatory (§I.5 rule 2) and unique per event type (enforced in every build), the order is **total and stable**: the same set of subscribers yields the same order regardless of registration sequence, construction timing, or platform. `try_subscribe` rejects a duplicate without mutating the registry; `must_subscribe` routes the same error to the all-build registration-error handler. Same-key registration order can never matter because same keys cannot enter the registry.

**Determinism guarantee, stated:** for a given build, the delivery order for event type E is a pure function of the set of live subscriptions to E — reproducible across runs, restarts, and (for the trace's benefit) describable in one registry snapshot. A headless test shuffles registration order across 100 permutations and asserts identical dispatch sequences.

Queued delivery composes with ordering per lane: when a pump delivers event E, that lane's subscribers to E run in key order within the delivery. (Subscribers on *different* lanes are ordered by their pumps' timing — cross-thread total order is out of scope by design; anything needing it shares a lane.)

### I.8 Observability — registry, trace, inspector

Core design goal, co-equal with the API: **you can always see what fired, who received it, and in what order.**

**The registry (who listens).** `hub.registry_snapshot()` returns, per event type: the ordered subscription list — name, phase, priority, lane/immediate, subscribe-site (file:line captured via macro), delivery count, cumulative and max handler time. This is the static answer to "who reacts to `bake.finished`, and in what order?" — available headlessly (dumpable from tests) and rendered in the inspector.

**The trace (what fired).** A ring buffer (default 16k records, per-hub) recording every emit: event name, payload summary (via the optional per-type serializer; type name only when absent), emitting thread + capture site, timestamp, and per-subscriber delivery records — subscriber name, immediate-or-lane, dispatch time, handler duration, and **nesting depth** for handler-emitted events, so cascades render as trees, not soup. Queued deliveries record both enqueue time and delivery time, making queue latency visible. Trace capture is a per-hub toggle; when off, overhead is one branch per emit (the bake_trace `current()` discipline, `bake-lab.md` §II.1).

**The inspector (in-editor).** A Bake Lab tab (the Lab shell already hosts pluggable tabs — `bake-lab.md` §II.5) with three views:

1. **Registry view** — event types × subscribers table, sorted by the ordering key, filterable by name namespace (`bake.*`), with the drop counters from §I.6 highlighted.
2. **Trace view** — live-tailing event log with nesting indentation, thread lanes, and payload expansion; pause/filter/pin.
3. **Timeline view** — event emissions and handler durations rendered on the existing BakeTrace flamegraph infrastructure (`bake_lab_timeline`): dispatches appear as spans (immediate handlers nest under the emitter's open span naturally, because dispatch can open a `bake_trace` span in the current collector). During a bake, this shows events interleaved with the phase spans that emitted them — one unified picture.

**flecs bridge:** ECS-side emissions (structural observers, entity gameplay events) mirror one record into the trace (name-prefixed `ecs.*`), so the inspector timeline is whole-engine even though dispatch is split per §I.4. Mirroring is trace-only — no double dispatch.

### I.9 Observable models — data-binding on the notification core

The second user-called-out requirement: values that multiple UI locations display and edit — a selected entity's transform mirrored in the gizmo, properties panel, and scene tree — need change *binding*, not hand-wired refresh. This is a distinct pattern from fire-and-forget events and gets first-class support layered on the core.

**What makes it distinct:**

| | Notification | Observable property |
|---|---|---|
| Nature | ephemeral occurrence | persistent current value |
| New subscriber | hears only future events | is **primed** with the current value on bind |
| Burst of N changes | N deliveries | **coalesced**: at most one delivery per flush, latest value wins |
| Payload | event struct | the value (+ old value where cheap) |

**API sketch:**

```cpp
// A model is a plain struct of properties; properties are bindable value holders.
struct SelectionModel {
    explicit SelectionModel(evt::PropertyScheduler& scheduler)
        : selected(scheduler, "sel.entity"),
          transform(scheduler, "sel.transform") {}

    evt::Property<EntityId>  selected;
    evt::Property<Transform> transform;
};

// Binding (properties panel). Primed immediately with the current value, then on change.
subs_ += model.transform.bind("props.transform_row",
                              [this](const Transform& t) { refresh_fields(t); });

// Mutation (gizmo drag). Marks dirty; observers coalesce to the flush point.
model.transform.set(new_t);

// Frame loop: one flush delivers at most one notification per dirty property.
property_scheduler.flush_dirty(); // app-thread; same frame-loop site as lane::app
```

**Coalescing semantics (the storm-prevention rule):** `set()` stores the value and marks the property dirty; observers are notified at the next `flush_dirty()`, once, with the latest value — a gizmo drag that mutates the transform 300 times per second notifies each observer once per frame. An opt-in `set_now()` exists for the rare must-see-every-step case and is trace-flagged. Equality suppression: `set()` with an unchanged value (per `operator==`) does not dirty.

**Threading and lifetime:** properties are **single-threaded by affinity** — each property belongs to its non-templated `PropertyScheduler`, and the scheduler is claimed and flushed by one thread (in practice: the app thread; models are a UI/editor pattern). This gives all `Property<T>` specializations one real flush registry without a templated-static singleton. Property destruction unregisters from the scheduler on that thread; the scheduler never retains a raw pointer past destruction. Cross-thread producers do not touch properties directly; they emit a queued notification, and an app-thread subscriber sets the property. This keeps properties lock-free and their callbacks reentrancy-simple, and it is a deliberate contrast with the hub (which is cross-thread by design): state-binding across threads hides races behind innocent-looking `.set()` calls, so the design forbids it rather than half-supporting it.

**Edit loops** (panel edits transform → gizmo updates → panel hears its own change): handled by the coalescing layer's re-entrancy rule — a `set()` from inside a property's own flush delivery is applied but deferred to the *next* flush, and equality suppression terminates ping-pong. The trace records property flushes like events (`prop.sel.transform`), so binding storms are visible in the same inspector.

**Relation to ECS data:** for entity-backed values the property is a *view-model*, not the source of truth — the ECS reconcile path stays authoritative (e.g. the existing `MarkLocalTransformDirty` observer, `transform_system.cpp:400`), and a thin adapter subscribes to the relevant change (flecs `OnSet` or a pulled snapshot) and sets the property. This spec defines the property layer; per-panel view-model adapters arrive with each migrated panel.

### I.10 The command layer — and the undo/redo foundation

Commands (taxonomy bucket 2) are built **on top of** the event system: they use the same transport (`Channel`), appear in the same trace, and their execution emits notifications — but they are deliver-once with a single handler, and they are the future home of undo/redo, so their shape matters now even though undo ships later.

**The abstraction:**

```cpp
struct Command {
    MT_COMMAND_NAME("workbench.open_part");   // named + namespaced, like events
    using Result = CommandResult<OpenPartReceipt>;
    // Parameterized: plain-struct payload, same rules as event payloads.
    std::string project, module;

    // Undo/redo shape (defined now, exercised later):
    //  - execute() is performed by the single registered handler;
    //  - a command that is *undoable* captures its inverse AT EXECUTE TIME,
    //    either as an inverse command or an opaque memento, and returns it;
    //  - non-undoable commands (bake requests, reload) return nothing.
};

struct CommandScopeToken {
    CommandScope kind = CommandScope::App; // enum class { App, ActiveSession }
    uint64_t session_id = 0;
    uint64_t generation = 0;
};

class CommandRegistry {
    // Exactly one handler per command name, enforced in every build.
    // Handler result may carry {value, error, inverse, coalesce_key}.
    template <class C>
    Registration must_register_handler(CommandScope scope,
                                       evt::Lane lane,
                                       Handler<C> h);

    // Owner-lane path: executes before returning and preserves typed results.
    template <class C> typename C::Result execute(C cmd);

    // Cross-thread/external path: queues once and returns a correlated completion.
    template <class C> CommandTicket<typename C::Result> dispatch(C cmd);
};
```

`must_register_handler` has an all-build fail-fast duplicate path matching `must_subscribe`; runtime-extensible code uses `try_register_handler` and handles `DuplicateHandler` without changing the registry. `execute` is legal only on the registered handler's lane and returns the typed result synchronously. It is the path for today's app-thread editor actions: create/duplicate still receive the created entity id immediately, delete/reparent still return validation failures immediately, and selection behavior does not change during migration. `dispatch` is the any-thread path for the external FIFO, worker requests, and future scripts. Its ticket contains a monotonic command id, exposes non-blocking `then(lane, callback)` completion, and permits `wait()` only off the handler's owner lane. Queue rejection, shutdown, supersession, stale session scope, handler failure, and success all complete the ticket exactly once; a caller is never left to infer whether a deliver-once command vanished. A ticket on a live but un-pumped lane stays pending until the owner pumps or a terminal cause fires — it is never dropped, and an off-lane `wait()` blocks until one of those happens.

**Design points that keep undo/redo reachable without redesign:**

- **The registry owns result finalization.** Both `execute` and queued `dispatch` enter the same internal execution path. The handler returns a typed result; before that result becomes visible to the caller/ticket, the registry records trace timing, transfers any inverse/coalescing metadata to its `HistorySink`, and emits `cmd.completed{id,name,status,duration}`. E4 installs a `NullHistorySink`; the later world-scoped `UndoStack` implements the same interface. No event subscriber is responsible for reconstructing or preserving the inverse, and adding undo does not change dispatch.
- **Inverse captured at execute time**, not construction time — "delete entity" can only know what to restore after it reads the entity; the handler returns the memento/inverse as part of its result. Undo pops and executes the stored inverse through the same internal path, with redo captured as the inverse-of-the-inverse.
- **Coalescing key** in the result: a gizmo drag emits many `edit.set_transform` commands; consecutive commands with the same key merge on the (future) undo stack into one undoable step — the property layer's coalescing (§I.9) handles *notification* storms, this handles *history* storms. The two compose: drag → many `set()`s → coalesced UI updates, plus one merged undo entry on release.
- **Typed completion plus generic observability:** the caller receives the typed result directly or through its ticket; the hub notification deliberately contains only generic trace fields (`id`, name, status, duration, scope token). Command **journaling** is a registry hook over the fully serialized command/result record, not a subscriber attempting to reconstruct typed results from the generic notification. Undoable editor actions — transform edit, entity create/delete, param change — become commands whose resulting scene deltas drive the UI, replacing today's direct calls.
- **Session scope is part of the envelope.** Each registration declares `App` or `ActiveSession`. For an active-session command, `execute`/`dispatch` stamp the current `SessionBinding` token. On world switch, `SessionBinding` closes the old epoch before replacing the session: new world-scoped submissions are rejected during the transition, queued old-epoch tickets complete as `StaleScope`, active handlers finish or are cancelled according to their command contract, handlers rebind, and only then does the new epoch open. Entity ids can therefore never drift into the next world. When the undo milestone installs an `UndoStack` history sink, its entries are keyed by the same token and clear in the close step.
- **Relation to `matter_async::CommandQueue`:** that queue's supersession semantics (`Reload` cancels the in-flight bake, `async_bake.h:55-58`) are exactly the deliver-once, cancellable contract — it *is* a command channel, specialized for the bake worker. It keeps its API and semantics, reimplemented on `Channel` (§I.5), and the facade-level commands that feed it (`viewer.reload` → session `Reload`) route through the registry so they are named, traced, and journal-able. Supersession is a per-channel policy (a push hook that cancels/clears), not a property of all commands — editor commands like `edit.set_transform` are never superseded. Superseded/cancelled commands do **not** produce undo entries (they never executed).
- **What is deliberately deferred:** the multi-command transaction API (group N commands into one undo step, e.g. duplicate-and-move) and the on-disk journal schema details. Undo-stack scoping and journal durability are already resolved in §II.3.

### I.11 Migration map

Every existing mechanism, its bucket, and its destination. "Compat shim" means the old API remains callable, reimplemented on the new core, until its consumers migrate.

| Existing mechanism | Bucket | Destination |
|---|---|---|
| Bake `Event` queue (`events.h`, `matter_engine.cpp:342,676,4638`) | Notification (queued) | Typed events `bake.started/part_done/finished/error`, `stream.refine_tile` on the session hub. New subscribers use `lane::app`; the legacy API has a dedicated `lane::legacy_poll` subscription set. `WorldSession::poll_event` pumps at most one legacy-lane envelope, converts it to the old `Event`, and returns it, so E3 needs no frame-loop pump and preserves one-event-per-call FIFO behavior. Drop counters replace the silent cap. |
| `BakeObserver` (`bake_observer.h`, W3) | Notification (immediate) | Ships as-is for W3. Later: `bake.mesh_ready` / `bake.rung_ready{level,tris,ms}` immediate events; the Workbench subscribes queued-to-app instead of maintaining its private mutex-guarded status log. Its thread-contract comment block becomes the §I.6 immediate contract's documentation seed. |
| `GpuJobQueue` (`async_bake.h:28`) | Neither — work submission | API unchanged; reimplemented on `evt::Channel` (donates `run_blocking`, budgeted `pump`, shutdown semantics to the shared primitive). Not hub events. |
| `CommandQueue` (`async_bake.h:56`) | Command | API unchanged; reimplemented on an unbounded `Channel` using `wait_pop`/`wait_pop_for` and an explicit supersession hook that completes displaced work as cancelled. Facade-level triggers become registered commands (§I.10). |
| `FileWatcher::poll` (`file_watcher.h:29`) | OS boundary | Keeps its poll interface (it *is* the OS edge); `LiveEditSession` republishes debounced results as `edit.file_changed` notifications. |
| Live-edit `ErrorSink` (`live_edit_interfaces.h:58`), `BridgeErrorSink` (`dynamic_scene_bridge.h:31`) | Notification | `error.live_edit{...}`, `error.part_instance{id,code}` / `error.part_instance_clear{id}` events. Sink interfaces become one-line adapters (construct with a hub, emit) during transition, then delete. |
| Viewer polled flags: `reload_requested`, `world_switch_requested` (`ui.h:73,102`), `WorkbenchHandoff` (`ui.h:51`), `focus_workbench_tab_` (`bake_lab.h:68`) | Command | Registered commands `viewer.reload`, `viewer.switch_world{index}`, `workbench.open_part{project,module}`, `lab.focus_tab{tab}`; handlers live where the poll-site code lives today (main loop, lab shell). Same-lane UI uses typed `execute`; FIFO/cross-thread sources use ticketed `dispatch`. World-targeting commands carry the `SessionBinding` scope token. The flags and handoff struct delete. |
| `PhysicsEvents` snapshot (`physics_context.cpp:1074`, `physics.h:107`) — orphaned | Notification (entity-shaped) | Delivered as **flecs entity events** during the pull stage on the tick thread (`phys.contact_begin/end`, `phys.sensor_enter/exit`, per-entity emit) — the gameplay-event foundation, finally wired. Aggregate per-step mirror into the hub trace for the inspector. The snapshot struct stays as the capture buffer. |
| flecs structural observers (`physics_systems.cpp:20`, `transform_system.cpp:400`, `streaming_systems.cpp:159`) | Notification (entity, tick-sync) | **Stay exactly as-is** (chosen approach C). Trace-mirroring optional, off by default for high-frequency structural events. |
| `PartResolver` (`scene_registry.h:65`), `Endpoint` (`sector_streaming_coordinator.h:97`), Box3D query callbacks | Strategy | **Stay as injected interfaces, permanently.** Excluded by semantics (§I.3.3). |

### I.12 Risks and mitigations

- **Reentrancy and lifetime bugs in immediate dispatch** (handler emits, unsubscribes, destroys its owner, or re-enters the emitter). Mitigated: shared callback control blocks, active check immediately before entry, non-blocking logical unsubscribe, explicit quiescent unsubscribe for cross-thread owners, all-build rejection of self-wait, a closed-hub state, emit-depth cap, and trace nesting (§I.6). Tests race emit against logical and quiescent teardown rather than assuming RAII is a synchronization barrier.
- **Ordering determinism regressions.** The (phase, priority, name) total order makes accidental order impossible because duplicate names never enter the registry in any build; `try_subscribe` returns an error and `must_subscribe` fails fast. The permutation and release-mode duplicate tests (§I.7) guard the invariant; the registry view makes the effective order reviewable at a glance instead of archaeologically.
- **Sync→queued migration breaks timing assumptions** — the highest-probability breakage: a consumer that ran inside `emit_event`'s caller now runs a frame later. Mitigated: migration is per-site and audited (§I.6); the default for every migrated site is to **preserve its current delivery thread and timing** (the bake queue was already queued-to-app, so its consumers see no change; the error sinks were synchronous, so they migrate as *immediate* first); any deliberate sync→queued change is its own reviewed step with the consumer's assumptions checked, never a side effect of the mechanical conversion.
- **Silent drops or accidental command loss** (today's 4096 cap, `matter_engine.cpp:678`). Mitigated by mandatory per-channel policy, out-of-band trace counters, keyed `CoalesceNewest` for state-shaped notifications, and non-dropping/reject-with-ticket-completion policies for commands. Drop diagnostics never recurse through the overflowing lane.
- **World commands execute against the wrong session after switch.** Mitigated by registry-stamped `CommandScopeToken`, a close/rebind/open epoch transition, cancellation of queued old-epoch tickets, and the same token on the future world-scoped undo stack (§I.10/§I.13).
- **Thread-affinity mistakes** (pumping a lane off-owner, touching properties cross-thread, GL calls from immediate handlers on the worker). Mitigated: `claim_lane` + debug asserts on pump and `Property::set` (the proven `assert_gl_thread` pattern); the immediate contract inherits `bake_observer.h`'s battle-tested wording; trace records emitting thread per event, so violations are diagnosable after the fact.
- **Two event systems confuse contributors** (flecs vs. hub). Mitigated: one routing rule (entity-scoped + tick-synchronous → flecs; else → hub), applied exhaustively in §I.11 so precedent covers every current shape; unified trace keeps runtime behavior inspectable in one place regardless of side.
- **Observability overhead distorts behavior.** Trace off = one branch per emit; ring buffer is fixed-size, no allocation per record beyond payload summaries (elided when off); handler timing uses the same cheap clocks as bake_trace, whose <0.1% overhead target (`bake-lab.md` §I.8) applies here too.
- **The hub becomes a new dumping ground** (everything stringly-typed, giant payloads, "misc" events). Mitigated: typed structs only (no generic variant payload API), namespaced names reviewed like public API, and the strategy-callback exclusion rule preventing the worst category error — request/response traffic masquerading as events.

### I.13 Hub topology, session lifetime, and SessionBinding

**Decision: one hub per `WorldSession`, plus one app (viewer/editor) hub.** Same `evt::Hub` type, multiple instances.

- **App hub** — editor/UI notifications, the command registry (§I.10), and observable models (§I.9). Lifetime = the editor. Subscribe once.
- **Session hub** — that session's bake/stream/entity/physics events. Born and dies with the session.

The app creates and owns its hub explicitly. `WorldSession` owns the session hub and exposes `evt::Hub& events()` / `const evt::Hub& events() const` from the public event interface; `SessionBinding` and the inspector never reach into `WorldSession::Impl`. The accessor's reference is valid only until session close/replacement, and app-side owners must quiesce their bridge subscription set before releasing that reference.

**Grounded in the session lifecycle:** a world **switch** *recreates* the session — `auto next = open_world(...); session = std::move(next)` (`MatterEditor/src/main.cpp:2004,2009`), destroying the old one — while a **reload** reuses it in place (`session->reload()`, `main.cpp:1998`). So switch kills a session, and a per-session hub is the right lifetime unit: the close protocol stops/joins session emitters, closes the hub (cancelling queued envelopes and waiting for in-flight callbacks), and only then destroys session state. A single global hub would instead require manually scrubbing every subscription/queued-event/trace-record tagged to the dead session on every switch *and* every workbench open/close — the exact cleanup this design exists to centralize.

**The rebind cost is real but bounded to one place.** Because switch recreates the session, anything app-side holding the old session pointer dangles *regardless of topology* — command handlers that mutate the world, view-model adapters, selection referencing a now-gone entity. A rebind step is therefore unavoidable in any design; per-session hubs don't add it, they just also cleanly kill the session-side half. That step is **`SessionBinding`**, living at the `complete_world_switch`/`open_world` seam. On session replace it:

1. closes the old command-scope epoch, rejects new world-scoped submissions during transition, and completes queued old-epoch tickets as `StaleScope`;
2. quiesces and tears down the app↔session bridge subscriptions — the small fixed set of editor-wide session-event subscribers (HUD, console, timeline) and the view-model adapters (§I.14);
3. clears app-side models referencing the dead world (selection, per-entity view-models) and, once installed, resets the world-scoped undo-stack history sink (§II.3.2);
4. replaces the session, rebuilds handlers/adapters against `session.events()`, snapshots the new scene model, opens a fresh command-scope epoch, and only then requests the new session's initial bake.

The ordering is part of the contract: the current `session = std::move(next)` seam moves inside `SessionBinding::replace`, and `open_world` no longer calls `request_bake` before handing the unopened-for-work session to the binding. Teardown happens **before** the old `WorldSession` destructor invalidates the public hub reference; the new worker cannot emit `bake.started` before the HUD/console/compat subscribers exist. Initial application startup uses the same bind-then-request order through `SessionBinding::initialize`. A failed replacement open leaves the old binding and epoch intact. A successful replacement cannot expose the new session to app commands until every bridge and model has rebound.

The **isolation session** (Part Workbench) gets its own hub too, and there the rebind cost is near-zero: its events only ever reach the workbench, which co-owns the session, so open/close is a self-contained teardown with no editor-wide rebinding.

**No automatic cross-hub forwarding.** Handlers reach across hubs by holding an explicit reference (an app-hub command handler calls into a session; `SessionBinding` explicitly bridges the session events the editor wants). Auto-propagation between hubs would reintroduce the emergent-order problem §I.7 works to kill. The **inspector** views multiple hubs via a hub selector — the same pattern as the Timeline's trace-source selector (`bake_lab_timeline`, task 2.2).

**Ruled out — single global hub with a session-source tag:** buys unified trace and zero rebind, but trades away RAII teardown (back to manual scrubbing of session-tagged state on every switch and workbench-close). Lifetime hygiene wins; the inspector's hub selector recovers the unified-view benefit at negligible cost.

### I.14 Worked example — the scene graph (app view-model over a session bridge)

The scene tree is the canonical case the topology is shaped around: a permanent editor panel whose entire content belongs to a session that is destroyed on every world switch. The editor already builds two-thirds of the target shape.

**Today** (`MatterEditor/src/editor_model.h`, `editor_model.cpp:26`, `main.cpp:989,1488`):

- `EditorModel` holds the flattened `HierarchyRow` list — an app-scoped view-model that survives world switches; the panel reads `editor.rows()`.
- `SceneCommands` is a struct of injected `std::function`s (`query_records`, `generation`, `create_empty`/`duplicate`/`delete_entity`/`reparent`) — a bridge decoupling the model from the session's ECS.
- Sync is actually an **unconditional full poll**: `main.cpp` calls `editor_model.refresh(scene_commands)` every frame, `EditorModel::refresh` always calls `query_records` and rebuilds the whole hierarchy, and `SceneCommands::generation` is the Flecs frame count. `WorldSession::graph_generation()` is unrelated: it tracks the provider's baked **part-graph snapshot** for other UI cards, not scene hierarchy mutation.
- Mutations flow back `EditorModel` → `SceneCommands` → session, returning `SceneEditResult`.

That is already an app-scoped view-model behind a session-bound bridge — the answer to "how does an editor-scoped view get its model from the active session" is *the bridge is the swappable indirection; the view-model and panel stay put*. The event system formalizes each piece and upgrades the weak one:

| Today | Under the event system |
|---|---|
| `EditorModel` rows | App-hub **observable model** (rows as an observable collection); panel binds once, primed on bind |
| `SceneCommands` `std::function` bridge | The **`SessionBinding`-owned adapter** (§I.13), rebuilt against the new session on switch |
| `query_records` | Adapter's full-snapshot path on bind/rebind and sequence-gap recovery |
| Flecs frame count passed as `generation` | Dedicated monotonic **scene-delta sequence** |
| **Per-frame full query + rebuild** | **Event-driven incremental sync** from canonical scene deltas |
| `create_empty`/`duplicate`/`delete_entity`/`reparent` (`SceneEditResult`) | Active-session commands using synchronous typed `execute` on the app lane; queued sources receive tickets |

**The missing source is added explicitly; the cited observers are not reused as if they already provided it.** `transform_system.cpp:400` only marks transforms dirty, and the physics observers only schedule reconciliation; neither reports all creates, deletes, names, parents, or component-list changes. E5 therefore adds a session-owned `scene::SceneChangeTracker`. Lightweight Flecs observers mark affected `SceneEntityId`s dirty for the supported editor-visible surfaces: entity add/remove, `ChildOf` add/remove, display-name identifier changes, and add/set/remove of components included in `SceneRecord::component_names`. At the end of the app-thread ECS tick, the tracker snapshots each dirty live entity once and publishes one of two canonical, typed session-hub events:

```cpp
struct SceneRowsUpserted {
    MT_EVENT_NAME("scene.rows_upserted");
    uint64_t sequence = 0;                 // increments once per published batch
    std::vector<scene::SceneRecord> rows;  // complete current record per affected id
};

struct SceneRowsRemoved {
    MT_EVENT_NAME("scene.rows_removed");
    uint64_t sequence = 0;
    std::vector<scene::SceneEntityId> ids; // includes descendants removed by cascade
};
```

Both are normalized cross-layer model deltas, not replacements for Flecs entity dispatch: Flecs remains the source observer mechanism, while the hub carries copied session→editor data. A single tick with both removals and upserts assigns consecutive sequence values in deterministic order (removals first, then upserts; ids sorted ascending inside each batch). The tracker also exposes `scene_snapshot(out_rows, out_sequence)` under the same app-thread affinity. `WorldSession::graph_generation()` and `graph_snapshot()` are never used for scene correctness.

**The adapter's recovery invariant:** `SessionBinding` snapshots `(rows, sequence)` when it binds, then accepts only `sequence == last_sequence + 1`. It applies an upsert/remove batch to the app observable collection and advances the sequence. A gap, duplicate with different contents, or unknown removal triggers one full `scene_snapshot`; it is logged and trace-flagged. This makes a bridge bug or interrupted rebind recoverable without pretending an unrelated generation counter protects the model. Because tracker flush and model update are both app-thread-affine, this adapter subscribes **immediate** and never queues or coalesces scene deltas; the property scheduler coalesces the resulting UI notifications at frame flush. Sequence values therefore cannot be skipped by a queue policy.

**On world switch:** `SessionBinding::replace` closes the old command epoch, quiesces the adapter subscription set while the old hub is still alive, clears the app model, closes/destroys the old session, binds to the new `session.events()`, and snapshots `(rows, sequence)` before opening the new epoch. The panel never changes binding — it still observes the same app-scoped model. This replaces the hand-rolled `Selection::world_generation` invalidation with the explicit session token from §I.10.

**Mutations close the loop through one supported path:** command handlers call a session `SceneService` for create/duplicate/delete/reparent/rename/component edits instead of mutating `session->ecs()` directly. The service performs validation and the Flecs mutation, returns the typed `SceneEditResult` to `execute`, and lets `SceneChangeTracker` publish the canonical delta at the end-of-tick flush. Create/duplicate can therefore select `created_id` immediately without hand-patching rows; the delta remains the only model-update path. Direct Flecs edits are still detected by the tracker's observers, but new editor code is required to use `SceneService` so validation, undo capture, and mutation semantics stay centralized.

**Threading:** scene queries, `SceneService`, `SceneChangeTracker::flush`, and the app observable model are app-thread-affine because the ECS tick runs in the app-thread frame loop (`session->tick`). An off-thread producer may only queue a scoped command or engine notification; it never mutates Flecs or a property directly. The app lane executes that work, the tracker emits its copied delta, and the adapter updates the property scheduler on the same thread. The frame-loop order is pinned: `session->tick` (which runs `SceneChangeTracker::flush`) precedes `PropertyScheduler::flush_dirty`, so scene deltas from tick N reach the UI in frame N rather than N+1 (today's `refresh`-before-`tick` order already carries a one-frame lag; E5 removes it by flushing after tick).

---

## Part II — Implementation Spec

Deliberately lighter than Part I: the design above is the deliverable to settle; milestones below are the expected shape of the work, sized for reordering once the design is approved. Each milestone is independently shippable and leaves the tree green.

### II.1 Milestones

**E1 — Core: `Channel` + `Hub` + tests (engine-only, no consumers).**
Public interfaces in `MatterEngine3/include/matter/event/`, implementations in `src/event/`, and `include/matter/events/` domain headers with `MT_EVENT_NAME`. Headless tests (`run-events`): subscribe/emit/logical unsubscribe; emit-vs-`unsubscribe_all_and_wait` race with a captured owner; close invalidation and late handle destruction; immediate + lane delivery; `try_pop`/blocking/timed pop; pump budget/progress guarantee; keyed coalescing that cannot replace another event type; caller-visible rejection outcomes; non-recursive drop diagnostics; all-build duplicate subscription rejection; ordering permutations (§I.7); reentrancy/depth cap; registry snapshot; trace ring buffer. Windows-runnable (no GL, no raylib — the `run-script`-class of test).

**E2 — Transport consolidation.**
`GpuJobQueue` and `CommandQueue` reimplemented on `Channel`, public APIs source-compatible: the GPU wrapper uses `pump`/`run_blocking`; the command wrapper uses unbounded `wait_pop`/`wait_pop_for` plus explicit token supersession. Existing async-bake behavior, timed refine wakeups, shutdown unblocking, and cancellation are verified by the current bake/streaming suites plus a supersession test proving every displaced token is cancelled. This is the "four queues become one primitive" payoff and the riskiest mechanical step — it lands alone, with nothing else in the diff.

**E3 — First real events + compat shim.**
Session hub in `WorldSession::Impl` with the public `WorldSession::events()` accessor; the ~40 `emit_event` sites convert to typed events. A private subscription set targets only `lane::legacy_poll`; `WorldSession::poll_event` claims that lane and `pump_one`s until it can return exactly one converted legacy `Event` or the lane is empty. It never depends on the E4 frame-loop app-lane pump and never dispatches unrelated subscribers. Error sinks convert to `error.*` events with adapter shims. Gate: all existing suites green with MatterEditor/tests untouched, legacy FIFO/one-call semantics tested, and drop counters visible.

**E4 — Inspector + viewer migration + SessionBinding.**
Events tab in the Bake Lab shell (registry/trace/timeline views with a hub selector, §I.8/§I.13); viewer polled flags → registered commands (`viewer.reload`, `viewer.switch_world`, `workbench.open_part`); `WorkbenchHandoff` and the flag fields delete; `MATTER_CMD_FIFO` routes through ticketed `dispatch` (§II.3.4). The registry lands with all-build unique-handler enforcement, same-lane typed `execute`, cross-thread tickets, result finalization/journaling hook, and `App` versus `ActiveSession` scope tokens (no undo stack yet). `SessionBinding` owns initial bind-before-bake and the close/quiesce/replace/rebind/open/request epoch sequence. Gate: reload/world-switch/open-in-workbench preserve existing result/timing behavior, queue-full/FIFO submissions always complete a ticket, a queued entity edit cannot cross a world switch, no initial bake event precedes binding, and session subscriptions quiesce before the old hub closes.

**E5 — Observable properties + the scene graph.**
`evt::PropertyScheduler` + `Property<T>` flush integration in the frame loop; the scene graph (§I.14) is the first real binding. A session `SceneService` centralizes supported mutations; `SceneChangeTracker` observes all editor-visible row inputs, emits sequenced canonical upsert/remove batches, and exposes an atomic `(snapshot, sequence)` recovery read. `EditorModel` becomes an observable collection behind the `SessionBinding` adapter, and scene mutations use typed `execute` (the first undoable candidates). Gate: property coalescing/equality/affinity/edit-loop tests; create/duplicate preserve immediate `created_id`; rename/reparent/component changes update only affected rows; cascade delete removes every descendant row; an intentionally skipped sequence triggers exactly one full resnapshot; world switch snapshots before opening its new command epoch.

**E6 — Entity events: wire the orphaned physics pipeline.**
`PhysicsEvents` → flecs entity events in the pull stage (`phys.*`); a consuming proof (debug overlay or test observer); trace mirroring. This is the gameplay-event foundation for Phase 6 scripting.

**Later (recorded, unscheduled):** `BakeObserver` re-expression on hub events; QuickJS `engine.events.on` binding (Phase 6); undo stack + coalesced editor commands; command journaling to disk.

### II.2 Touched files, summary

| File | Milestone | Change |
|---|---|---|
| `MatterEngine3/include/matter/event/{channel,event_hub,subscription}.h`, `src/event/*` | E1 | new — public transport/hub handles; private implementation and callback control blocks |
| `MatterEngine3/include/matter/events/*.h` | E1/E3 | new — typed event declarations per domain |
| `MatterEngine3/tests/event_tests.cpp`, `tests/Makefile` | E1 | new — `run-events` (headless, Windows-runnable) |
| `MatterEngine3/src/async_bake.{h,cpp}` | E2 | `GpuJobQueue`/`CommandQueue` on `Channel`; APIs source-compatible |
| `MatterEngine3/include/matter/world_session.h`, `src/matter_engine.cpp` | E3 | session hub + public accessor; typed emits; dedicated legacy-poll lane/shim; close ordering |
| `MatterEngine3/src/live_edit*`, `src/ecs/dynamic_scene_bridge.*` | E3 | error sinks → `error.*` events via adapters |
| `MatterEditor/src/event_inspector.{h,cpp}`, `bake_lab.*` | E4 | new — Events tab (registry/trace/timeline) |
| `MatterEngine3/include/matter/event/command.h`, `src/event/command.cpp` | E4 | registry; typed execute/ticketed dispatch; scope epochs; result finalization |
| `MatterEditor/src/session_binding.{h,cpp}`, `main.cpp`, `ui.{h,cpp}`, `asset_browser.*` | E4 | app hub ownership; epoch-safe session replacement; flags/handoff → commands; frame-loop pump |
| `MatterEngine3/include/matter/event/property.h`, `src/event/property.cpp` | E5 | new — scheduler-backed observable properties |
| `MatterEngine3/src/scene/{scene_service,scene_change_tracker}.{h,cpp}` | E5 | supported scene mutations; canonical sequenced row deltas and snapshot recovery |
| `MatterEditor/src/editor_model.*`, `scene_tree_panel.*` | E5 | observable row collection; incremental adapter; full snapshot only on bind/gap |
| `MatterEngine3/src/ecs/physics_context.cpp`, `ecs_runtime.cpp` | E6 | `PhysicsEvents` → flecs entity events in pull stage |

### II.3 Resolved decisions

The five questions posed at design time, resolved with the user:

1. **Hub topology → per-`WorldSession` hub + one app hub**, with a `SessionBinding` rebind at the switch seam. Rationale, session-lifecycle grounding, and the ruled-out global-hub alternative are in §I.13; the scene graph works through this split in §I.14.
2. **Undo-stack scoping → app-owned command registry, world-scoped stack keyed by the active-session token, cleared while closing the old epoch.** Undoing an edit to a world no longer loaded is meaningless, so the stack resets before handlers rebind. *Not* per-panel (a duplicate-and-move must be one undo step) and *not* global-across-worlds. The workbench isolation session is not an undo target — its "undo" is re-selecting a prior pinned variation, already provided.
3. **Trace/journal persistence → reuse BakeTrace's JSON format.** Event-trace export is on-demand (dump the ring buffer when asked, not always-on streaming). Command journaling is a distinct opt-in *persistent* log that reuses the same serialization — same format, two durability policies.
4. **`MATTER_CMD_FIFO` → route through ticketed command-registry `dispatch` in E4.** It is a cross-thread command source by definition; routing it makes external commands named/traced/journaled, gives every submission an explicit completion, and deletes a bespoke poll path. Dev-convenience commands (screenshot) are simply non-undoable app-scoped commands.
5. **E3 cadence → domain-by-domain**, `bake.*` first (already queued-to-app, lowest timing risk, proves the shim), then `stream.*`, then the error sinks (the synchronous ones needing the careful sync→immediate call). A 40-site big-bang would rubber-stamp the §I.6 per-site delivery-mode audit the migration depends on.

**Still genuinely open (accommodated by the design, scheduled later):** multi-command transactions (group N commands into one undo step, e.g. duplicate-and-move) — the command shape supports it via a transaction wrapper, but the grouping API and nesting rules are deferred to the undo-stack milestone; and the on-disk command-journal schema specifics (beyond "reuses the trace serialization").

### II.4 Adversarial-review corrections

The seven issues found in the post-design adversarial review are resolved as contracts, not left as implementation judgment calls:

1. **Cross-thread subscriber lifetime:** RAII performs logical removal; shared callback control blocks plus `unsubscribe_all_and_wait` provide the explicit quiescence barrier; session shutdown joins emitters before `Hub::close` (§I.5–I.6).
2. **Command results and inverse ownership:** same-lane `execute` returns typed results, queued `dispatch` returns a correlated ticket, and the registry finalizes results/captures inverses before completion (§I.10).
3. **Scene graph factual/source gap:** the spec now records the real unconditional poll, separates part-graph generation from scene state, and adds `SceneService` + sequenced `SceneChangeTracker` deltas with snapshot recovery (§I.14).
4. **Incomplete transport contract:** `Channel` now includes non-blocking, blocking, and timed consumer operations plus caller-visible push outcomes; command channels have explicit non-dropping policies (§I.5).
5. **Commands surviving a world switch:** active-session command envelopes are epoch-stamped, old tickets are completed stale during `SessionBinding::replace`, and the new epoch opens only after rebind/snapshot (§I.10, §I.13).
6. **E3 compatibility dead zone:** `poll_event` owns and pumps a dedicated legacy lane one envelope at a time, independent of the E4 app-lane frame pump (§I.11, II.1 E3).
7. **Debug-only uniqueness:** duplicate subscription names and command handlers are rejected in every build; `must_*` registration fails fast without inserting the duplicate (§I.5, §I.7, §I.10).
