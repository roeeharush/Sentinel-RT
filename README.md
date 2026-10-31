# SentinelRT

A C++17 software simulation and monitoring platform, inspired by embedded, real-time and
system-testing concepts. Simulated embedded-style devices produce sensor measurements; a
monitoring system receives them over TCP, validates them, processes them concurrently, detects
faults and reports what it found. It is *real-time oriented*: it uses periodic sampling, timeout
detection and latency measurement, and it measures its timing behaviour rather than assuming it.
It was built step by step, with unit tests, sanitizer runs and measured performance.

## Status

All planned milestones are implemented. As of 2026-10-16:

- 256 tests pass (251 unit tests and 5 scenarios), checked from a fresh clone in Release, and in the
  AddressSanitizer + UndefinedBehaviorSanitizer and ThreadSanitizer builds, with no sanitizer reports.
- The Python benchmark automation has its own 122 unit tests.
- Measured results and their limitations are in the Benchmarks and Results sections.

## What it does

- **Simulates devices** with temperature, pressure and voltage sensors that sample periodically,
  and can inject faults on purpose (`Spike`, `Drop`, `Stuck`, `Drift`, `Noise`, `Invalid`).
- **Sends measurements over TCP** from a device simulator process to a monitoring server process.
- **Processes measurements concurrently**: a bounded queue feeds a pool of worker threads.
- **Detects problems**: threshold violations, a stuck sensor and a drifting value (sliding
  window), invalid measurements, and devices that stop sending (timeout).
- **Runs scripted scenarios** deterministically and checks the outcome against expectations.
- **Measures its own performance**: throughput and latency (a benchmark program plus a Python
  script that automates the runs).

## Programs

| Program | Purpose |
|---|---|
| `monitor_app` | Single-process version: a simulated device feeds a bounded queue and a worker pool, with threshold detection (no network) |
| `monitor_server` | TCP server (one thread per connection) that receives measurements and runs the pipeline: bounded queue, worker pool, threshold and sliding-window detection, timeout monitoring |
| `device_sim` | TCP client that simulates a device and can inject a fault |
| `scenario_runner` | Runs a `.scenario` file deterministically and checks its expectations |
| `benchmark` | Measures the processing pipeline and prints one result line |

## Architecture

Data flow between the two processes, `device_sim` and `monitor_server`:

```
  device_sim  (one process)
  +-----------------------------------+
  |  simulated device                 |
  |  temperature, pressure, voltage   |
  |  (a fault can be injected)        |
  +-----------------+-----------------+
                    |
                    |  TCP, port 5000
                    |  length-prefixed frames
                    v
  monitor_server  (another process)
  +-----------------------------------+
  |  connection thread, one per       |
  |  client: read a frame, decode it  |
  +-----------------+-----------------+
                    |
                    v
  +-----------------------------------+
  |  BoundedQueue  (capacity 1024)    |
  +-----------------+-----------------+
                    |
                    v
  +-----------------------------------+
  |  WorkerPool  (4 worker threads)   |
  +-----------------+-----------------+
                    |
                    v
  +-----------------------------------+
  |  MeasurementProcessor             |
  |    - judge: threshold + sliding   |
  |      window                       |
  |    - statistics                   |
  |    - timeout monitor              |
  |    - logger                       |
  +-----------------------------------+

  Alongside: one more thread wakes every 500 ms and reports
  devices that have been silent for more than 2 seconds.
```

How a measurement travels:

1. A simulated device samples its sensors, and `device_sim` sends each measurement as a frame over TCP.
2. The server runs one thread per connection. It reads the frame, decodes it and pushes it into the queue.
3. One of four worker threads pops it and passes it to `MeasurementProcessor`.
4. The processor judges it (threshold and sliding window), counts it, records that the device was seen, and logs a problem if there is one.
5. Separately, a thread wakes every 500 ms and reports devices that have been silent for more than 2 seconds.

- **Producers and consumers.** Each connection thread pushes decoded measurements into a bounded
  queue; worker threads pop and process them. When the queue is full, `push` blocks, so a slow
  consumer slows the producer down instead of losing data.
- **Timeouts.** `TimeoutMonitor` records when each device last sent something. A separate thread
  checks every 500 ms and reports devices silent for more than 2 s.
- **Shared processing code.** The per-measurement work lives in `MeasurementProcessor`, which is
  used by both `monitor_server` and the `benchmark` program, so the benchmark measures the same
  work the server does.

### Network protocol

TCP is a byte stream and does not keep message boundaries, so every message is framed:

- **Frame:** a 4-byte big-endian length, followed by that many payload bytes.
- **Payload:** text, `deviceId|sensorCode|timestampSeconds|value`, for example `17|TEMP|1720001112|73.5`.
  Sensor codes are `TEMP`, `PRES` and `VOLT`.
- **Limits and errors:** a frame larger than 65536 bytes closes the connection. A frame whose
  payload cannot be decoded is logged and discarded; the connection stays open.

Components are organised under `src/common/` by role: `core` (basic types), `sensors`,
`detection`, `pipeline`, `network`, `infra` (clock, logger, statistics, histogram), `config`
(argument parsing and scenarios) and `benchmark`.

## Requirements

- **Linux.** The networking code uses POSIX sockets, so it does not build on native Windows. It was
  developed on Ubuntu 26.04 under WSL2.
- **A C++17 compiler.** Tested with g++ 15.2.0.
- **CMake 3.20 or newer.** Tested with 4.2.3.
- **git and an internet connection** the first time CMake configures the project: it downloads
  GoogleTest 1.15.2.
- **Python 3** for the benchmark scripts (standard library only). Tested with 3.14.4.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

If `CMAKE_BUILD_TYPE` is not given, the project builds as Debug. Use Release for anything you
want to measure.

## Run

The executables are in `build/src/apps/`.

**Server and device.** Start the server in one terminal. It listens on port 5000 (fixed in the code).

```bash
./build/src/apps/monitor_server
```

In a second terminal, start a simulated device. It connects to `127.0.0.1:5000`, and retries up to
5 times, one second apart, if the server is not up yet.

```bash
./build/src/apps/device_sim --cycles 20 --device-id 7 --fault Spike --fault-after-ms 500
```

| `device_sim` option | Meaning | Default |
|---|---|---|
| `--cycles N` | Number of sampling cycles | 10 |
| `--period-ms N` | Time between cycles, in milliseconds | 100 |
| `--device-id N` | Device identifier sent with every measurement | 1 |
| `--fault TYPE` | Fault to inject on the temperature sensor: `Spike`, `Drop`, `Stuck`, `Drift`, `Noise`, `Invalid` | none |
| `--fault-after-ms N` | Delay before the fault starts (needs `--fault`) | 0 |

Several `device_sim` processes with different `--device-id` values can talk to the same server. Each
process simulates one device. The server logs problems it detects, and logs a device that stops
sending as timed out. About once a second it prints a `[stats]` line with the totals since it started:
the number of measurements, how many were ok, warning, critical or invalid, how many missed the 20 ms
deadline, and the largest latency in microseconds. The latency of a measurement is the time from the
moment the server decodes it to the end of its processing, so it is the wait in the queue plus the
processing (the same definition as `total` in the benchmark). The server stamps each measurement
itself, because the protocol timestamp has a resolution of one second.
Stop the server with Ctrl+C (or `kill`, which sends SIGTERM). It stops accepting
connections, waits for its threads and exits with code 0 (see Limitations for the details).

**Scenarios.** Run scripted scenarios, with no network. The exit code is 0 if every expectation
holds, 1 if one fails and 2 for bad input.

```bash
./build/src/apps/scenario_runner scenarios/*.scenario
```

**Single-process version:**

```bash
./build/src/apps/monitor_app --cycles 50 --period-ms 100
```

The `benchmark` program and its Python script are described in the Benchmarks section.

## Test

```bash
cd build && ctest
```

This runs the unit tests and the five scenarios. The Python script has its own tests:

```bash
python3 -m unittest discover -s scripts -p "test_*.py"
```

**Sanitizers.** The same test suite can be built with AddressSanitizer and UndefinedBehaviorSanitizer,
or with ThreadSanitizer (use a separate build directory for each):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j4 && (cd build-asan && ctest)
```

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j4 && (cd build-tsan && ctest)
```

## Benchmarks

### What is measured

The `benchmark` program runs the processing pipeline (bounded queue, worker pool,
`MeasurementProcessor`) inside one process. A feeder thread plays the devices (10 simulated devices by
default) and pushes measurements into the queue. **There is no network in this benchmark**: it
measures what happens from the moment a measurement is in the queue.

| Mode | What the feeder does | What it is for |
|---|---|---|
| `max` | Pushes as fast as the queue allows; a full queue blocks it | Finding the highest throughput |
| `rate` | Pushes at a fixed rate | Latency at a chosen load |
| `calibrate` | Same feeding, but the workers only count | Checking the feeder and queue, with no real work |

Each run has a warm-up (discarded), then the measured part. The program prints one line of
`key=value` fields. Two latencies are recorded, each in a histogram with 1 microsecond buckets up to
100 ms:

- `total`: from the push into the queue until processing ends (waiting plus processing);
- `service`: processing only.

`scripts/run_benchmarks.py` runs the program several times per point, takes the **median**, refuses any
result that is not a Release build, and writes `results.csv` (every raw run) and `results.md` (summary,
environment, limitations) into a new timestamped folder under `benchmarks/results/`. Nothing is overwritten.

```bash
python3 scripts/run_benchmarks.py --mode max --workers 1 2 4 6 8
```

### Method

| Item | Value |
|---|---|
| CPU | AMD Ryzen 5 3500X, 6 cores (6 logical cores) |
| OS | Ubuntu on WSL2 (Linux 6.18.33.2-microsoft-standard-WSL2) |
| Compiler | g++ 15.2.0, Release build |
| Devices | 10 |
| Per run | 5 s measured, after a 1 s warm-up |
| Runs per point | 3 (median reported; min and max shown) |
| Date | 2026-10-16 |

The raw data is in `benchmarks/results/`:

| Folder | Content |
|---|---|
| `2026-10-16T12-34-37` | Calibration, 1 to 8 workers |
| `2026-10-16T12-41-37` | `max` mode, **before** the queue change described below |
| `2026-10-16T12-45-17` | 2 workers only, 5 runs of 10 s (checking the result for 2 workers) |
| `2026-10-16T12-57-27` | `max` mode, **after** the queue change |
| `2026-10-16T16-45-18` | `rate` mode, 100,000 items/s |
| `2026-10-16T16-46-48` | `rate` mode, 500,000 items/s |
| `2026-10-16T16-48-18` | `rate` mode, 800,000 items/s |
| `context-switches-2026-10-16.txt` | Context switches per item, before and after |

## Results

### Before: throughput does not grow with workers

`max` mode, median of 3 runs of 5 s:

| Workers | Throughput (items/s) | Min to max | `total` p50 (us) |
|---|---|---|---|
| 1 | 936,065 | 910,007 to 1,184,482 | 814 |
| 2 | 141,198 | 124,314 to 147,117 | 7,362 |
| 4 | 653,211 | 625,365 to 717,154 | 1,509 |
| 6 | 753,790 | 748,194 to 792,969 | 1,269 |
| 8 | 892,775 | 826,194 to 895,189 | 1,081 |

Two workers were by far the slowest, which was not expected. The first suspicion was contention on
the shared mutexes and the logger. The measurements did not support that as the main cause.
Five runs of 10 s with 2 workers gave a median of 148,320 items/s (133,228 to 162,679), so the
result is not noise.

### What was found

When the queue is full, the producer sleeps waiting for space. In `pop()`, every removed item called
`notify_one()` on the producer. The producer woke up, pushed one item, filled the queue and went back
to sleep, so a sleep and a wake-up happened for almost every item. Counting voluntary context
switches per processed item (`context-switches-2026-10-16.txt`, 3 runs per point):

| Workers | Before the change | After the change |
|---|---|---|
| 1 | 0.075 to 0.079 | 0.003 to 0.004 |
| 2 | 0.36 to 0.40 | 0.013 |
| 4 | 0.135 to 0.158 | 0.076 to 0.094 |

The counts come from this command, run 3 times per worker count (the value used is the line
`Voluntary context switches`, divided by the `sent` field of the output):

```bash
/usr/bin/time -v ./build/src/apps/benchmark --mode max --workers 2 --devices 10 --duration-ms 5000 --warmup-ms 1000
```

**The change:** `pop()` now wakes a blocked producer only when the queue has fallen to half of its
capacity or less. An empty queue always counts, so a producer can never be left asleep next to an
empty queue. Three tests cover it, and the first one failed before the change.

### After

Same command, same machine, same session:

| Workers | Throughput (items/s) | Min to max | `total` p50 (us) | `service` p99 (us) |
|---|---|---|---|---|
| 1 | 2,232,080 | 1,980,512 to 2,244,995 | 333 | 1 |
| 2 | 2,373,245 | 2,371,802 to 2,396,158 | 300 | 2 |
| 4 | 1,066,124 | 1,025,548 to 1,154,665 | 646 | 52 |
| 6 | 1,051,249 | 974,330 to 1,057,165 | 676 | 65 |
| 8 | 923,645 | 917,990 to 981,806 | 617 | 109 |

With 2 workers, throughput went from 141,198 to 2,373,245 items/s. Changing only when the producer is
woken removed the anomaly, so this was the cause.

### Latency at a fixed rate

In `max` mode the queue is always full, so `total` mostly reflects the queue size. To measure latency
the feeder was run at a fixed rate (`rate` mode), after the queue change. Same machine, 10 devices,
3 runs of 5 s per point, medians. In all 15 points the rate achieved was within 0.1% of the
requested rate.

`max lag` is the largest number of measurements by which the feeder was behind its own schedule.

**100,000 items/s**

| Workers | `total` p50 (us) | p95 | p99 | max | `service` p99 | Queue max | Max lag (items) |
|---|---|---|---|---|---|---|---|
| 1 | 16 | 34 | 48 | 358 | 3 | 26 | 100 |
| 2 | 20 | 65 | 101 | 804 | 27 | 132 | 286 |
| 4 | 32 | 107 | 147 | 744 | 40 | 171 | 308 |
| 6 | 45 | 120 | 164 | 1,869 | 44 | 191 | 508 |
| 8 | 43 | 121 | 169 | 1,537 | 52 | 154 | 576 |

**500,000 items/s**

| Workers | `total` p50 (us) | p95 | p99 | max | `service` p99 | Queue max | Max lag (items) |
|---|---|---|---|---|---|---|---|
| 1 | 29 | 73 | 209 | 4,178 | 2 | 1,024 | 1,904 |
| 2 | 63 | 439 | 743 | 10,021 | 12 | 1,024 | 14,617 |
| 4 | 186 | 750 | 975 | 7,751 | 56 | 1,024 | 5,803 |
| 6 | 288 | 860 | 1,066 | 7,880 | 73 | 1,024 | 8,835 |
| 8 | 271 | 811 | 1,043 | 18,290 | 87 | 1,024 | 10,794 |

**800,000 items/s**

| Workers | `total` p50 (us) | p95 | p99 | max | `service` p99 | Queue max | Max lag (items) |
|---|---|---|---|---|---|---|---|
| 1 | 44 | 247 | 587 | 9,570 | 1 | 1,024 | 7,545 |
| 2 | 94 | 553 | 955 | 17,259 | 4 | 1,024 | 23,740 |
| 4 | 353 | 817 | 992 | 7,858 | 56 | 1,024 | 7,237 |
| 6 | 482 | 891 | 1,084 | 22,229 | 69 | 1,024 | 21,177 |
| 8 | 559 | 965 | 1,104 | 4,701 | 95 | 1,024 | 16,972 |

What these numbers show:

- **Measured:** at every rate, latency grew with the number of workers. At 100,000 items/s the
  median `total` was 16 us with 1 worker and 43 to 45 us with 6 or 8; at 500,000 items/s it was 29 us
  with 1 worker and 271 to 288 us with 6 or 8. This matches the throughput results: on this machine
  more workers were not better for either metric.
- **Measured:** latency grows as the rate approaches the ceiling. With 1 worker the median went
  from 16 us (100,000 items/s) to 44 us (800,000 items/s), and the p99 from 48 us to 587 us.
- **Caution:** at 500,000 and 800,000 items/s the queue reached its capacity of 1,024 in every
  configuration, and `max lag` was in the thousands of items (14,617 items at 500,000 items/s is about
  29 ms of schedule). The feeder was therefore stalled at times, and the latency of the measurements
  that were held back is not counted (coordinated omission). The p95, p99 and max values at those
  rates are optimistic. The cause of the stalls was not investigated.
- **100,000 items/s** is the cleanest point: the queue stayed small and `max lag` was 100 to 576 items
  (1 to 6 ms of schedule).

### What the results say, and what they do not

- **Measured:** more than 2 workers did not increase throughput on this machine; from 4 workers up it
  fell (about 1.07 million, 1.05 million and 0.92 million items/s). The `service` tail grows with the
  worker count (p99 of 1, 2, 52, 65 and 109 us).
- **Not investigated:** why throughput falls from 4 workers up. Possible causes are the single mutex
  of the queue, the mutexes of the detectors and the logger. None of them was tested.
- **Calibration** (`2026-10-16T12-34-37`) gave 3,046,618 items/s with 1 worker, so the feeder can
  supply far more than the pipeline processed, and the feeder was not the bottleneck there. With 2 to 8
  workers it gave 114,277 to 225,485 items/s. That is not a ceiling for those worker counts, because
  with workers that only count the queue is mostly empty, which is a different regime from `max`.
- **The comparison above is for a saturated queue** (`queue_avg` of 570 to 750 out of 1024 in the
  "after" run). Under a lighter load the queue is not full and the change is expected to matter much
  less; that was not measured.

### Limits of these measurements

- One machine (6 cores), under WSL2. The CPU frequency, the Windows power plan and the background
  load were not controlled. Results can differ by tens of percent between sessions (for example 1
  worker gave 936,065 items/s in the "before" run and about 1.05 to 1.11 million in the context-switch
  runs), so compare numbers from the same session only.
- Latency is stamped when a measurement is pushed. If the feeder is blocked, the latency looks better
  than the real wait (coordinated omission). In `max` mode `max lag` was 0, but `total` there mostly
  reflects the queue size and is not the latency of a real system. In `rate` mode `max lag` was not
  zero (see above), so those latencies are optimistic at the higher rates.
- Percentiles are the upper edge of a 1 microsecond bucket, and values above 100 ms are counted as
  overflow.
- The context-switch counts are from one process-wide counter, including the warm-up second, and were
  not part of the saved `results.md` files.
- The number of devices was fixed at 10 and the network is not part of the benchmark, so these results
  say nothing about how many real devices the server can handle.

## Design decisions

| Decision | Why | Alternative and trade-off |
|---|---|---|
| **Bounded queue with a blocking `push`** between the connection threads and the workers (`monitor_server`) | A slow consumer slows the producer instead of losing measurements, and memory stays bounded | `pushDropOldest` (used only by `monitor_app`) never blocks but loses data. Blocking means a full queue stops a connection thread from reading its socket |
| **One thread per connection** | Simple to write and to reason about | A thread pool or `epoll` scales to more connections. The number of connections is not limited here, and the benchmark does not cover the network, so the cost is not measured |
| **Length-prefixed frames, text payload** | TCP has no message boundaries, and a length prefix needs no escaping. A text payload is easy to read and to test by hand | A binary payload is smaller and faster to parse, but harder to inspect. A delimiter would need escaping |
| **Threshold detector plus a sliding window** (10 values) | Easy to explain and test. The window catches what a threshold cannot: a stuck sensor (all values within 0.01) and a drift (the average of the second half differs from the first by more than 1.0) | Statistical or learned detectors could do more, but are harder to explain and test |
| **Time is passed in as a parameter** (`IClock`, `ManualClock`, `TimeoutMonitor::checkTimeouts(now)`) | Timeout logic is tested with exact times and no `sleep` | A little more plumbing |
| **Scenarios use computed timestamps and one worker** (`scenario_runner`) | The run takes no real time and gives the same result every time, so a failing expectation is a real failure | It checks the logic, not the behaviour of a real clock or the network. The network path has been run by hand with real processes, but there is no automated test for it yet |
| **`MeasurementProcessor` is shared** by `monitor_server` and `benchmark` | The benchmark measures the same work the server does, and the two cannot drift apart | The benchmark still excludes the network |
| **Latency histogram with 1 microsecond buckets** (100,000 buckets, atomic counters) | No allocation and no lock on the hot path, and a percentile is cheap to read | Resolution is 1 us and the range is 100 ms; a percentile is the upper edge of its bucket |
| **The benchmark script refuses Debug results, takes the median of several runs and never overwrites a result folder** | A number from a Debug build, one noisy run or an overwritten file would be misleading | The script is stricter and a run takes longer |
| **`pop()` wakes a blocked producer only at half capacity or below** | Waking it on every item cost a sleep and a wake-up per item (see Results) | Under a saturated queue the producer wakes later. Its effect under a light load was not measured |

## Limitations

**What it is.** A software simulation, not real hardware and not a hard real-time system: it runs on a
general-purpose OS under WSL2 and gives no timing guarantees. It needs Linux (POSIX sockets).

**Server and device simulator**

- The address and port (`127.0.0.1:5000`), the worker count (4), the queue capacity (1024), the
  timeout settings and the detector thresholds are constants in the code. There is no configuration file.
- Shutdown is graceful but simple. Ctrl+C or SIGTERM only sets a flag. The accept loop, the timeout
  thread and every connection thread check it (the blocking calls have a 200 ms receive timeout so that
  they can). The server then joins those threads and stops the worker pool, which finishes the
  measurements already in the queue. In two manual runs with one silent client connected it exited with
  code 0 after 0.04 s and 0.50 s; the longest wait is the 500 ms sleep of the timeout thread.
- During shutdown a client that connects at the last moment is not served and is disconnected, and a
  message that is only partly received is discarded. The server keeps every connection thread in a list
  until shutdown, so the list grows with each connection, even after the connection has closed.
- One thread per connection, with no limit on the number of connections.
- Timeout detection only knows devices that have sent at least one measurement.
- A `device_sim` process simulates one device, and a fault can be injected only into its temperature
  sensor, one fault per run.
- The drift threshold is a fixed 1.0 in each sensor's own unit, the same for temperature, pressure and
  voltage. The window size is fixed at 10.
- The protocol timestamp has a resolution of one second, so the server does not use it to measure
  latency. The server latency does not include the time on the device or on the network, so it is not
  an end-to-end latency, and the two machines' clocks are not synchronised in this project. The 20 ms
  deadline is a constant chosen as an example, not a requirement.
- The `[stats]` line reads each counter separately, so the numbers in one line may differ by one
  measurement that was counted between two reads. It is for monitoring, not for exact accounting.

**Input checking**

- The message decoder checks that a number was read, but not that the whole field was used, so a field
  such as `17x` is read as 17. The command-line parser of `device_sim` accepts a value such as `12abc` in
  the same way.
- `monitor_app` keeps its own copy of the status-to-log-level mapping and uses only the threshold
  detector, so it does not detect a stuck or drifting sensor.
- `BoundedQueue::push` does not check whether the queue is closed. It is safe today because of the
  order in which components are stopped: the server joins the connection threads before it stops the
  worker pool. A connection thread blocked in `push` on a full queue would wait forever if the workers
  stopped first.

**Measurements**

- The Benchmarks section lists the limits of the measurements: one machine under WSL2, results that vary
  between sessions, optimistic latency at high rates (coordinated omission), a fixed 10 devices and no
  network in the benchmark.
- No automated test runs the server and `device_sim` as real processes (the unit tests cover the
  parts, and the whole path was run by hand).
- Not measured: the effect of the queue change under a normal (not saturated) load, the network path,
  more than 10 devices, and the cause of the fall in throughput from 4 workers.

## Future work

These follow from the limitations above. None of them is implemented.

1. **Find why throughput falls from 4 workers up.** Test the suspects one at a time (the queue mutex,
   the detector mutexes, the logger) and measure again after each change.
2. **Measure the queue change under a normal load** by running `rate` mode before and after it.
3. **Benchmark the network path** with many `device_sim` processes, now that each can have its own
   `--device-id`.
4. **Find out why the feeder stalls at high rates** (the large `max lag`), so that the latency at those
   rates can be trusted.
5. **Configuration:** read the port, worker count and thresholds from a configuration file. Also remove
   finished connection threads from the list while the server runs, and add an automated test that
   starts the server as a process and stops it with a signal.
6. **Stricter parsing** of numeric fields, and support for several faults at once.
