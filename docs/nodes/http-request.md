# HTTP Request

One node for a call to a web API: a webhook, a REST lookup, an upload. It comes in the pack
[examples/packs/io.github.bmaltais.fugue-http](../../examples/packs/io.github.bmaltais.fugue-http), which you install like any pack
(`fugue pack add examples/packs/io.github.bmaltais.fugue-http`, or Settings > Packs). The question shows that the pack
declares **network**, and nothing leaves your machine until you say yes. It needs `python3` on the path and nothing else.
The exact rules are in the [spec](../spec.md#613-http-request-first-party-pack).

What it does, in short:

- **https only**, except `localhost`, `127.0.0.1` and `::1`, where plain http works. Nothing is sent otherwise.
- It runs **only when a run reaches it**. Checking or validating a workflow never calls anything. The node always runs when
  the workflow runs (it is never served from the cache), and a node with nothing after it still runs.
- It stops after **30 seconds** (`timeout_s`) and refuses an answer over **5 MB** (`max_response_mb`).
- A status of 404 or 500 is **not an error**: `status` is an output, so the workflow can branch on it. Turn on
  `fail_on_error_status` to make 400 and above stop the run.
- It does not follow redirects: a 302 comes back as the status and body.
- **No keys in workflows.** A workflow file is plain text and gets shared. Keep a key in the pack's secrets instead:
  open Settings > Packs, find *HTTP Request*, write the service's address under *Only for* (`https://api.example.com`), type the key
  and press Enter (or pipe it in: `read -rs v; printf %s "$v" | fugue pack secret set io.github.bmaltais.fugue-http token_1 --origin https://api.example.com`).
  Then write `{secret:token_1}` in a header (`Authorization: Bearer {secret:token_1}`) or in the path or query of the URL. `token_1` to `token_3` are there.
  **A key goes only to the address it was set for**: a workflow that points the node at another host (or builds the host from an input, or puts
  a placeholder in the address) stops with "token_1 is only for https://api.example.com" before anything is sent, so opening a workflow you were given cannot leak your key. The value is filled in when the node runs,
  and is taken out of everything the node returns, so a server that echoes it back shows `***`. It cannot go in the body (use a header),
  and an input that holds the text `{secret:token_1}` sends that text, not the key. A secret that is not set stops the node with
  `secret_missing` and says where to set it.

## Inputs and outputs

| Input | What it is |
| --- | --- |
| `method`, `url` | `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, and the address. |
| `headers` | One `Name: value` per line. |
| `body_kind`, `body` | `none`, `text` (as it is) or `json`. |
| `a`, `b`, `c`, `d` | Text that fills `{a}` to `{d}` in the URL, the headers and the body. Wire a text output into them. |
| `picture`, `audio`, `video` | The first one set is uploaded as `multipart/form-data` in the field `upload_field` (default `file`). A JSON object body becomes the other form fields. |
| `path1` to `path3` | What to pick out of a JSON answer. |

Outputs: `status`, `body` (the answer as text), `field1` to `field3` (what the paths picked; left empty when the path finds nothing).

Placeholders in the **JSON** body go inside strings, because the body is read as JSON first and the values are put in after, so a
quote or a line break in a value can never break it:

```json
{"content": "Render {a} is done", "count": "{b:json}"}
```

`{b:json}` as a whole string becomes the JSON value in `b` (`42`, `true`, `[1,2]`); `{a}` anywhere else is text. In the URL a value
is percent-encoded.

**JSON path.** Dots for keys, `[n]` for list items, `[-1]` for the last, `["key"]` for odd names, an optional `$` first:
`data.items[0].name`, `$.results[-1].id`, `meta["total count"]`. No wildcards or filters. A string comes back as it is, a number or
boolean as text (`3`, `true`), an object or list as compact JSON.

## Example 1: a webhook POST

Post a message to a Discord-style webhook when a render finishes. Wire the text you want into `a`.

| Input | Value |
| --- | --- |
| `method` | `POST` |
| `url` | `https://discord.com/api/webhooks/<id>/<token>` (the address is the credential: keep the workflow private, or paste the address at run time with `fugue run --set`) |
| `body_kind` | `json` |
| `body` | `{"content": "Render finished: {a}"}` |

The node sends `Content-Type: application/json`, and `status` is `204` when the hook took it. To attach the picture too, wire it into
`picture`: the body's keys become form fields and the picture goes up as a file.

## Example 2: a JSON lookup

Look up a value and use it later. This asks a public API for a country and picks two fields:

| Input | Value |
| --- | --- |
| `method` | `GET` |
| `url` | `https://restcountries.com/v3.1/name/{a}?fields=name,capital` |
| `a` | `Canada` (or a wired text) |
| `path1` | `[0].name.common` |
| `path2` | `[0].capital[0]` |

`field1` is `Canada`, `field2` is `Ottawa`; wire them into a prompt or a Save name. If the country is unknown the service answers 404,
`status` is `404` and the fields are empty, so a later node can check `status` instead of the run failing.

## Testing it

`tests/cli/test_http_request_node.cpp` runs the node against a fake server on the loopback (JSON, placeholders, paths, multipart,
timeout, size cap, https-only, secrets, and that a validation never calls it). The tests never reach the internet.
