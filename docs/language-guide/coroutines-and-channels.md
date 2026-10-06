# Coroutines and channels

Whisky coroutines are stackless and execute cooperatively on the calling thread. Tasks run when resumed with `sip(handle)`. Yielding uses `drop(value)`, and cancellation uses `cancel(handle)`.

## Tasks

Tasks can be defined as named functions using `fn drip` or as anonymous task blocks with `brew { ... }`:

```wky
import stdc;

fn drip counter() {
    defer println("counter cleanup");
    drop(1);
    drop(2);
    return 3;
}

fn i32 main() {
    handle task = counter();
    println("sip 1: {sip(task)}");
    println("sip 2: {sip(task)}");
    println("sip 3: {sip(task)}");
    println("sip 4: {sip(task)}");
    cancel(task);
    return 0;
}
```

Output:
```
sip 1: 1
sip 2: 2
sip 3: 3
sip 4: 3
counter cleanup
```

A completed task retains its final return value across subsequent calls to `sip`.

A task begins in a suspended state. If a task is canceled before its first resumption, owned arguments are destroyed without running unregistered body defer statements. Canceling a task after a suspension point executes the defers and owned values active at that yield in last-in, first-out order. Control flow statements like `return`, `break`, `continue`, yields, and blocking channel operations cannot escape from inside a defer block.

### Anonymous tasks: `brew`

```wky
import stdc;

chan<i32> comms = make_chan(2);

fn i32 main() {
    handle worker = brew {
        comms <- 100;
        drop(1);
    };

    sip(worker);
    let val = <-comms;
    println("received: {val}");

    cancel(worker);
    close_chan(comms);
    release(comms);
    return 0;
}
```

Tasks created with `brew` can read module-level globals, but do not capture local variables from their enclosing function scope. Data is passed into tasks via `fn drip` parameters or through channels.

Task handles are managed, move-only resources. They can be stored in structs, arrays, and channels. Destroying an owning handle cancels the task automatically. Handles cannot be cloned.

## Channels: `chan<T>`

Channels provide FIFO message queues for coroutines.

```wky
import stdc;

chan<i32> queue = make_chan(2);

fn i32 main() {
    handle producer = brew {
        queue <- 10;
        queue <- 20;
        drop(0);
    };

    sip(producer);

    let first = <-queue;
    let second = <-queue;
    println("first={first} second={second}");

    cancel(producer);
    close_chan(queue);
    release(queue);
    return 0;
}
```

`make_chan(capacity)` allocates a managed ring buffer. The capacity parameter must be positive. Physical allocation rounds up to a power of two for bitmask indexing, while logical capacity matches the requested size.

### Channel properties

Channels expose read-only metadata fields:
* `.cap`: Logical maximum capacity.
* `.count`: Current number of queued items.
* `.head`: Current read position index.
* `.mask`: Bitmask used for circular indexing.
* `.closed`: Boolean indicating if the channel has been closed.

### Channel operations and lifecycle

* Sending (`chan <- msg`): Evaluates the message expression once. If the channel is full inside a coroutine, the coroutine suspends until space is available. Sending to a full channel outside a coroutine traps.
* Receiving (`let msg = <-chan`): Evaluates the channel once. If empty inside a coroutine, the task suspends until an item is available. Receiving from an empty channel outside a coroutine traps.
* Closing (`close_chan(chan)`): Prohibits new sends. Queued items can still be drained. Sending to a closed channel, or receiving from an empty closed channel, traps.
* Deallocation (`release(chan)`): Destroys the channel ring and cleans up any unread owned messages.

Channels coordinate cooperative tasks on a single thread and do not synchronize multi-threaded operating system threads.

## Multiplexing: `select`

The `select` statement polls multiple channel operations in declaration order:

```wky
import stdc;

fn i32 main() {
    chan<i32> ch1 = make_chan(2);
    chan<i32> ch2 = make_chan(2);

    ch2 <- 42;

    select {
        case v = <-ch1: {
            println("ch1: {v}");
        }
        case v = <-ch2: {
            println("ch2: {v}");
        }
        default: {
            println("no channel ready");
        }
    }

    close_chan(ch1);
    close_chan(ch2);
    release(ch1);
    release(ch2);
    return 0;
}
```

Output:
```
ch2: 42
```

`select` evaluates channel expressions once. The first ready channel executes its block. If no channel is ready, the `default` block executes immediately. Inside a coroutine without a default case, the task suspends until one of the polled channels becomes ready. Up to eight cases are supported in a single select statement.
