# Performance tests

Send a request many times, varying its parameters, and watch its throughput, response times and statuses live: a
load tester (in the spirit of wrk, oha or k6) built into Zalie, on top of the aeronet HTTP client.

![The Performance tab after a run](../assets/screenshots/performance.png){ .zalie-shot }

## Open a Performance tab

- **Performance**, the button next to **Fetch** (or ++ctrl+shift+enter++).
- Or right click a request of the sidebar: **Performance test**.

The Performance tab opens next to the request it is bound to. It shows the request as written (its method and URL with
the `{{variables}}`), the settings of the run on top and the dashboard below; the separator between them is draggable.
Edits of the request in its own tab apply to the next run. ++ctrl+enter++ runs the active Performance tab and ++esc++
stops its run.

Performance tabs are not restored after a restart: export a run you want to keep (see [Export](#export-a-run)).

## Run settings

| Setting | Meaning |
| ------- | ------- |
| Connections | Each connection is a thread sending its next request as soon as the previous one is answered (the closed model of wrk). 0 uses the number of vCPUs, which is also the limit, unless raised in **Settings > Performance runs** |
| Duration | The run ends after this time, warm-up included. 10 s by default |
| Warm-up | The requests started during the first seconds are sent but not counted: connections are opened, caches warmed |
| Max requests | The run ends after this number of requests, warm-up included |
| Max req/s | The requests never go faster than this, whatever the number of connections. With the adaptive rate: its maximum |
| Adaptive rate | The rate adapts to what the server sustains, see [below](#adaptive-rate) |
| Min req/s | Adaptive rate only: the rate it starts from and never goes below. 10 when 0 |
| Max p99 | Adaptive rate only: the rate backs off when the p99 of the response times exceeds this. 0 for no cap |
| One pass | The run also ends once every value of the parameters has been sent |
| Combine | How the parameters advance together, see [below](#combine-the-parameters) |
| Seed | Equal seeds send equal requests (timestamps aside). 0 picks a new seed for each run |

For the limits, 0 means none, and the first limit reached ends the run. The line below the settings tells what the run
will send ("Planned: 2000 requests on 8 connections", "adaptive rate 10 to 5000 req/s", "one pass: 150 requests"), or
what prevents it: an empty range, a
`{{variable}}` without value, a missing data file...

The settings are saved in the collection, next to the request, so they can be shared and versioned with it. They are
left out of the collection file until a value is changed.

## Parameters

Every `{{variable}}` of the request gets a row of the parameter table (also those referenced by the values of other
variables), and so does every query parameter of its URL: no need to write `{{...}}` to vary `page=1`. The body, or
fields of a JSON body, can be varied too (see [Vary the body](#vary-the-body)).

| Column | Content |
| ------ | ------- |
| Parameter | The variable, the query parameter (marked "query", or "added" when only the run sends it), the body ("whole") or a JSON field ("JSON"). A name in orange has a problem, told below the settings |
| Source | **Default** sends what a normal Fetch sends: the value of the environment or of the collection, or the value written in the URL for a query parameter. Any other source varies the values for the run |
| Values | A summary of the values |
| Order | In order, at random, or shuffled (each value once per pass, in a random order) |
| Count | The number of distinct values, "unbounded" for random strings, UUIDs and timestamps |
| Samples | The first values the run will send |

Varying a query parameter only applies to the run: the request keeps its value, a normal Fetch keeps sending it.
The same goes for the buttons at the end of the rows:

- The cross of a query parameter leaves it out of the run (source **Not sent**). Its row stays, dimmed, and its plus
  sends it again.
- **+ Query parameter**, below the table, adds a query parameter that the request does not have: type its key in its
  row, choose its source, and the run appends it to the URL. Its cross removes it.
- **+ Body** and **+ JSON field** vary the body, see [below](#vary-the-body).

Click a row to edit its source below the table:

| Source | Settings | Example |
| ------ | -------- | ------- |
| Fixed value | the value | `en` |
| List | one value per line, typed or pasted | `dog`, `cat`, `bird` |
| Integer range | from, to, step, digits | `1` to `50`, `00001` to `99999` with 5 digits |
| Decimal range | from, to, step, decimals | `0.5` to `2` by `0.25` |
| Date range | from, to, step (seconds to days), format | each day of January 2026 as `2026-01-31` |
| Random string | length, characters | 8 lower-case letters |
| UUID | | a random version 4 UUID, derived from the seed |
| Timestamp | Unix seconds, milliseconds or ISO 8601 | the time of the request |
| Data file | path, format, column | `users.csv`, column `id` |
| Not sent | | a query parameter of the request left out of the run |

A range starts valid: from the value of the query parameter when it fits (`page=3` gives 3 to 102, `dates=2026-12-18`
gives a week from that day, in the same shape), otherwise from its defaults, today for dates.

The dynamic variables (`{{$randomUUID}}`, `{{$timestamp}}`...) are evaluated again for every request.

Below the editor, **First requests** lists the first requests as they will be sent, with their values encoded where
they land: percent-encoded in the URL, form-encoded in a form, escaped inside a JSON string.

### Vary the body

The body can vary without writing `{{...}}` in it, for the run only: the request keeps its body.

- **+ Body** (a JSON, Text or XML body) replaces the whole body. A body on one line becomes the first value of a
  **List**: add one body per line, or take them from a data file (a body on several lines starts as a fixed value).
  The values are sent as they are.
- **+ JSON field** (a JSON body) replaces the values that a JSONPath selects: `$.user.name`, `items[*].id` (the
  leading `$` is optional, see [Filtering a JSON body](responses.md#filtering-a-json-body) for the syntax). A string
  stays a string, the value is escaped inside its quotes; a number, a boolean, `null`, an object or an array is
  replaced by the value as it is, so a list of `1`, `2.5`, `{"a": 1}` sends them as JSON. A `{{variable}}` written in
  place of a value (`{"page": {{page}}}`) counts as a value. Several fields can vary at once, each with its own source.

The row of a JSON field shows its value in the request, and a range starts from it (`"count": 5` gives 5 to 104).
A JSON field whose path matches nothing, two parameters replacing the same part of the body (the whole body and one of
its fields, a field and one inside it), or a body that is not valid JSON are told below the settings. Its cross
removes the row. **First requests** shows the bodies as they will be sent.

### Data files

When there are many values, keep them in a file: plain text (one value per line), CSV with a header, or JSON Lines.
Type its path, or **drop the file on its row**. A path inside the directory of the collection is saved relative to
it, so that the collection stays portable. The file is never loaded in memory: it is indexed in the background (the
editor shows its number of rows, its columns, its first rows and its first malformed line), then the connections read
the rows they need. Several rows reading the columns of the same file stay together: a `user` goes with its `token`.

### Combine the parameters

- **Independent** (default): each parameter advances on its own. One pass covers the longest of them.
- **Together**: paired by position, the first values with the first values, like the columns of a file. One pass
  covers the shortest of them.
- **All combinations**: every combination, the last parameter advancing first, like nested loops (3 terms x 50 pages
  = 150 requests for one pass).

With **Together** and **All combinations**, the order of the first parameter applies to all of them.

## Run

**Run** (or ++ctrl+enter++) starts the run. Before a run against another host than the local machine (`localhost`,
`127.0.0.0/8`, `::1`), or with a method that may change data (anything but `GET`, `HEAD`, `OPTIONS` and `TRACE`), a
confirmation tells the host, the method and the planned volume.

**Stop** (or ++esc++) ends the run at once: no new request is sent, the requests in flight are not counted. Closing
the tab stops its run too, without waiting for the requests in flight.

A run whose first requests all fail without a response (connection refused, unknown host, TLS error, timeout), the
warm-up included, stops by itself after one failure per connection (3 to 10 of them), and tells why: the server cannot
be reached. Once a response was received, failures are measures and the run goes on.

During a run the dashboard refreshes about 10 times per second, not at the rate of the screen: the interface leaves
the CPU to the connections, and is idle again once the run is over. The status bar tells how many runs are in
progress.

<div class="zalie-hero" markdown>

![Zalie eating her meat](../assets/zalie-eat.gif){ width="240" }

</div>

Meanwhile Zalie eats a big piece of meat beside the status line, as fast as she can. The meat goes as the run
progresses (by time or by requests, whichever limit is closer), so the bare bone means the run is nearly over; without
a limit, a new piece comes every 8 seconds. Once the run is over, she naps. She chomps at the pace of the dashboard
refreshes: the animation costs no extra frame.

## Adaptive rate

Check **Adaptive rate** to let Zalie find the rate the server sustains instead of fixing it. The rate starts at
**Min req/s**, a rate the server surely handles, then changes after each measure (at least 30 requests, or 5 seconds
at low rates: every second once the rate reaches 30 requests per second), like the congestion control of TCP. How it
looks for the highest healthy rate depends on **Max req/s**:

- **No maximum** (0): the rate **doubles every second** while the server stays healthy, until the first sign of load.
- **A maximum**: a **bisection** between **Min req/s** and **Max req/s**, faster to get high. The rate tries the
  middle of the range, on a logarithmic scale (between 10 and 100 000, it tries 1 000 first): healthy, the middle
  becomes the low bound, otherwise the high bound. Once the two bounds are within 10 % of each other, the search is
  over and the rate goes on from the low one. From 10 to 100 000 requests per second, about 8 measures.

Then, as with any measure:

| The last measure | The rate |
| ---------------- | -------- |
| Healthy: response times close to their baseline, no more failures than usual | doubles while ramping up, tries the next middle while bisecting; afterwards, rises by 5 % |
| Slower: median above 1.5 times its baseline, p90 above 2 times, or p99 within 10 % of **Max p99** | holds |
| Overloaded: median above 3 times its baseline, p90 above 4 times, or p99 above **Max p99** | backs off by 20 % |
| Failing: failures (`5xx`, `429`, no response) 1 point above their usual share, or over half of the requests; no response at all | halves |
| Not reached: the connections send less than 90 % of the rate | comes down to what they reach |

It never goes below **Min req/s** nor above **Max req/s** (0: no maximum), and keeps adapting until the run ends: if
the server weakens later, the rate follows it down.

- **Baseline**: the lowest median and p90 of the healthy measures, made at low load first. Each comparison adds a
  margin of 0.5 ms, the noise of a fast local server. The usual share of failures is the one of the healthy measures:
  an endpoint failing 5 % of the time at any rate is not overloaded.
- **Max p99**, in milliseconds, for a service level ("p99 under 50 ms"). 0 relies on the baseline only.
- A rise is judged at the next second already, against the rates before and after it (the rate changes a fraction
  of a second after the end of the second it was decided on). A drop skips the next second, which still shows the
  load of the higher rate.
- The warm-up runs at the minimum. Give the run time: without maximum, from 10 to 10 000 requests per second takes
  about 13 seconds of doubling (3 seconds for the first 30 requests at 10 per second). A higher **Min req/s** gets
  there sooner. The chart of the requests per second has a linear scale: an exponential rise looks flat at first.
- The connections still bound the rate, each one waiting for its response before sending the next: when they cannot
  reach it, the dashboard says so. Add connections, or the rate measures the client rather than the server.
- The dashboard tells the state of the rate (ramping up, searching by bisection with its two bounds, probing, holding,
  backing off because the server is overloaded or because requests fail, limited by the connections, at its maximum),
  its target and the baseline median. **Requests / s** gives the **sustained** rate, the highest one
  measured while the server was healthy, and the chart of the requests per second draws the target over them.

## Dashboard

- **Status line**, beside Zalie: phase (warming up, running, finished), measured time, connections, requests in
  flight and seed, then the progress of the run. A
  warning appears when the client, not the server, is the bottleneck (its CPU is above 90 % of what its connections
  can use): the numbers then measure Zalie more than the server.
- **Key numbers**: requests per second (current during the run, mean once over, and the peak; the sustained rate
  instead of the mean with the [adaptive rate](#adaptive-rate)), median, p99 and maximum response time, share of `2xx`
  and of errors, decoded throughput and requests counted.
- **Requests per second**, stacked by status class: a wave of `5xx` shows at once. With the adaptive rate, its target
  is drawn over them.
- **Response time**: p50, p90, p99 and max of each second, on a linear or logarithmic scale.
- **Latency distribution**: how many responses (the vertical axis) took each time, on a logarithmic axis, the
  percentiles marked.
- **Latency by percentile**: the time under which 50 %, 90 %, 99 %, 99.9 % of the responses were received.
- **Status distribution**: one bar split by class (green `2xx`, blue `3xx`, orange `4xx`, red `5xx`, dark red for the
  requests without response), then the count and share of each status code and of each kind of error (connection,
  TLS, timeout, closed, malformed, too large). Hover an error for its first message.
- **Response times** table: count, min, mean, standard deviation, percentiles and max, to the end of the body and to
  the response head, and per status class: fast errors would otherwise flatter the percentiles.

On the charts, hover to read the values of a second for every series, click a name of the legend to hide or show its
series. Once the run is over, drag over a chart over time to zoom on a window of it, and double click to show
everything again.

Response times are measured from the send of each request to the end of its body (or to its head), with a log-linear
histogram (about 6 % of precision at most) per connection. Bodies are counted, never kept nor analyzed: the run
measures the server, not Zalie.

## Export a run

**Export JSON...** writes the whole run to a file, `runs/<request>-<date>.json` in the [Zalie home](../reference/file-format.md)
by default: the request, the settings and the seed (to send the same requests again), the data files, the totals, the
percentiles, the histograms, the statuses, the errors and the time series. The format is described in
[Files and formats](../reference/file-format.md#run-reports).

## Good to know

- Each connection is a thread with its own HTTP client: hundreds of connections need hundreds of threads. Many
  streams multiplexed on a few HTTP/2 connections wait for the asynchronous client of aeronet.
- The HTTP version and the TLS settings of the request apply: set **HTTP version** to compare HTTP/1.1 and HTTP/2 on
  the same server.
- A long run keeps the time series of its last hour.
- Coming next (see the [Roadmap](../ROADMAP.md#performance-tab)): open model with a target rate, a new connection per
  request, bodies sent from files, breakdown of the latency by value of a parameter, slowest and failed samples, saved
  runs and their comparison, assertions and thresholds, and a command line runner.
