# Variables and environments

Write `{{name}}` in the URL, the parameters, the headers (names and values), the body, the form fields or the
authentication: Zalie replaces it when the request is sent (and in the code snippets).

## Where values come from

From the highest to the lowest priority:

1. the **active environment** of the collection (selector of the top bar),
2. the **variables of the collection**,
3. the **dynamic variables** below.

A variable may reference others (`baseUrl = {{scheme}}://{{host}}`), up to 8 levels deep (which also stops reference
cycles). An undefined variable is sent verbatim, and reported in orange under the URL bar and in the **Variables** card
of the inspector, which also tells where each value comes from. Values of variables named like secrets (`token`,
`password`, `apiKey`...) are masked there until hovered.

## Dynamic variables

| Variable | Value |
| -------- | ----- |
| `{{$timestamp}}` | Unix time in seconds |
| `{{$timestampMs}}` | Unix time in milliseconds |
| `{{$isoTimestamp}}` | ISO 8601 UTC time, `2026-10-29T13:37:00.123Z` |
| `{{$randomInt}}` | random integer between 0 and 1000 |
| `{{$randomUUID}}`, `{{$guid}}` | random UUID (version 4) |
| `{{$env.NAME}}`, `{{process.env.NAME}}` | the environment variable `NAME` of the Zalie process |

The `process.env.NAME` form is the one of Bruno: imported collections keep working. Keeping secrets in the
environment of the process (`TOKEN=... zalie`) keeps them out of the collection files.

## Environments

An environment is a named set of variables: typically `local`, `staging` and `prod`, sharing the same names
(`baseUrl`, `token`...) with different values. Create and edit them in the collection settings; the active one is
remembered per collection.
