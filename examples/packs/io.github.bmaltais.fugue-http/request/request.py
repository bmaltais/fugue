"""HTTP Request: a one-shot Fugue command node (Python 3 standard library only).

One JSON request on stdin, one result or error line on stdout (docs/nodes/protocol.md). It runs only
when a run reaches the node; a check or validation never starts it. Rules, in the order they apply:

- Only https is called, except loopback (localhost, 127.0.0.1, ::1), where http is also allowed.
- {secret:token_1} to {secret:token_3} name the pack's own secrets (#402). The runner puts only those the pack declared in the
  request's "secrets" object, at run time. A secret fills the URL path or query (percent-encoded) and header values, never the body or the address, and
  only for the origin (scheme, host, port) the person set it for. Its value is taken out ("***") of everything the node returns.
- A redirect is not followed: its status and body come back as they are.
- A status of 400 or more is an output, not an error, unless fail_on_error_status is on.
- The whole call (connect, send, read) must finish within timeout_s, and the answer may be at most
  max_response_mb megabytes.
"""

import ipaddress
import json
import mimetypes
import os
import re
import secrets
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

JSON_PLACEHOLDER = re.compile(r"\{([a-d]):json\}")
ANY_PLACEHOLDER = re.compile(r"\{([a-d])(:json)?\}")
ANY_OR_SECRET = re.compile(r"\{(?:([a-d])(:json)?|secret:([a-z][a-z0-9_]*))\}")
SECRET = re.compile(r"\{secret:[^{}]*\}")
SECRET_PLACEHOLDER = re.compile(r"\{secret:([a-z][a-z0-9_]*)\}")
DECLARED_SECRETS = ("token_1", "token_2", "token_3")  # pack.json's settings
MIN_MASKED = 4
HEADER_NAME = re.compile(r"^[!#$%&'*+.^_`|~0-9A-Za-z-]+$")
LOOPBACK_NAMES = {"localhost"}
UPLOADS = ("picture", "audio", "video")
METHODS = ("GET", "POST", "PUT", "PATCH", "DELETE")  # the manifest's choices


class Failure(Exception):
    def __init__(self, code, message, hint=""):
        super().__init__(message)
        self.code = code
        self.text = message + (" " + hint if hint else "")


def fail(code, message, hint=""):
    raise Failure(code, message, hint)


def is_loopback(host):
    if host.lower() in LOOPBACK_NAMES:
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def check_no_secret(text, where):
    if SECRET.search(text):
        fail("bad_input", "%s uses {secret:NAME}, and a secret cannot go there." % where,
             "Put it in a header or the URL; a body is what a server often echoes back.")


def origin_of(scheme, host, port):
    """scheme://host[:port], lower case, the default port left out: the one form an origin is compared in."""
    scheme = scheme.lower()
    host = host.lower()
    if ":" in host:
        host = "[%s]" % host
    standard = 443 if scheme == "https" else 80
    return "%s://%s%s" % (scheme, host, ":%d" % port if port and port != standard else "")


def template_origin(template):
    """The origin of a URL template, read from its literal scheme and authority. A placeholder there ({a} to {d},
    {secret:...}) is refused: it would let a workflow decide where a key goes."""
    match = re.match(r"^([A-Za-z][A-Za-z0-9+.-]*)://([^/?#]*)", template)
    if not match:
        fail("bad_input", "The URL must start with https:// and a host written out.", "Placeholders go in the path or the query, not in the address.")
    if "{" in match.group(0) or "}" in match.group(0):
        fail("bad_input", "The scheme, user, host and port of the URL cannot hold a placeholder.",
             "Write the address out, like https://api.example.com/path/{a}; only the path and the query can use {a} to {d} or {secret:...}.")
    try:
        parts = urllib.parse.urlsplit(match.group(0))
        host, port = parts.hostname, parts.port
    except ValueError:
        fail("bad_input", "The URL's address is not valid.")
    if not host:
        fail("bad_input", "The URL must start with https:// and name a host.")
    return origin_of(parts.scheme, host, port)


class Secrets:
    """The pack's secret values for this run, and the one place they are looked up and masked. A value is used only for the
    origin the person set it for, which is checked against the URL's own (written out, no placeholders) before any connection."""

    def __init__(self, given):
        self.values = {}
        self.origins = {}
        for name, entry in (given or {}).items():
            if isinstance(entry, dict) and isinstance(entry.get("value"), str) and entry["value"]:
                self.values[name] = entry["value"]
                self.origins[name] = entry.get("origin") if isinstance(entry.get("origin"), str) else ""
        self.request_origin = None

    def value(self, name):
        if name not in DECLARED_SECRETS:
            fail("bad_input", "{secret:%s} is not one of this pack's secrets." % name[:40],
                 "Use " + ", ".join("{secret:%s}" % each for each in DECLARED_SECRETS) + ".")
        if name not in self.values:
            fail("secret_missing", "The secret %s is not set." % name,
                 "Set it under Settings > Packs, or `fugue pack secret set io.github.bmaltais.fugue-http %s --origin https://api.example.com`." % name)
        origin = self.origins.get(name, "")
        if not origin:
            fail("secret_missing", "The secret %s has no origin, so it is not used." % name,
                 "Clear it and set it again with the origin it is for, like https://api.example.com.")
        if origin != self.request_origin:
            fail("bad_input", "%s is only for %s, and this URL goes to %s." % (name, origin, self.request_origin),
                 "A key is sent only to the service it was set for. Change the URL, or set the key for this address.")
        return self.values[name]

    def forms(self, value):
        quoted = urllib.parse.quote(value, safe="")
        lowered = re.sub(r"%[0-9A-F]{2}", lambda m: m.group(0).lower(), quoted)
        return {
            value, quoted, lowered, urllib.parse.quote_plus(value), lowered.replace("%20", "+"),
            json.dumps(value)[1:-1], json.dumps(value, ensure_ascii=False)[1:-1],
        }

    def mask(self, text):
        found = set()
        for value in self.values.values():
            if len(value) >= MIN_MASKED:
                found |= self.forms(value)
        for form in sorted(found, key=len, reverse=True):
            text = text.replace(form, "***")
        return text

    def mask_all(self, node):
        if isinstance(node, str):
            return self.mask(node)
        if isinstance(node, list):
            return [self.mask_all(item) for item in node]
        if isinstance(node, dict):
            return {self.mask(key): self.mask_all(item) for key, item in node.items()}
        return node


def fill_text(template, values, quote=False, secrets=None):
    """Replaces {a} to {d} (and {a:json}, as the text it holds) and, where `secrets` is given, {secret:NAME} in one pass,
    so a value is never expanded again (an input that holds the text {secret:token_1} stays that text)."""

    def one(match):
        if match.re is ANY_OR_SECRET and match.group(3) is not None:
            value = secrets.value(match.group(3))
        else:
            value = values[match.group(1)]
        return urllib.parse.quote(value, safe="") if quote else value

    pattern = ANY_OR_SECRET if secrets is not None else ANY_PLACEHOLDER
    return pattern.sub(one, template)


def fill_json(node, values):
    if isinstance(node, str):
        whole = JSON_PLACEHOLDER.fullmatch(node)
        if whole:
            raw = values[whole.group(1)]
            try:
                return json.loads(raw)
            except ValueError:
                fail("bad_input", "Input %s is not JSON, and the body uses {%s:json}." % (whole.group(1), whole.group(1)))
        return fill_text(node, values)
    if isinstance(node, list):
        return [fill_json(item, values) for item in node]
    if isinstance(node, dict):
        return {fill_text(key, values): fill_json(item, values) for key, item in node.items()}
    return node


def check_secret_names(text, where):
    for match in SECRET.finditer(text):
        if not SECRET_PLACEHOLDER.fullmatch(match.group(0)):
            fail("bad_input", "%s has a {secret:...} that does not name a secret." % where,
                 "Write it like {secret:token_1}: lowercase letters, digits and _.")


def parse_headers(text, values, secrets):
    headers = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        name, colon, value = line.partition(":")
        name = name.strip()
        if not colon or not HEADER_NAME.match(name):
            fail("bad_input", "Header line %r is not Name: value." % name[:40])
        filled = fill_text(value.strip(), values, secrets=secrets)
        if "\r" in filled or "\n" in filled:
            fail("bad_input", "The value of header %s has a line break after the placeholders are filled." % name)
        headers[name] = filled
    return headers


def has_header(headers, name):
    return any(key.lower() == name for key in headers)


def multipart(fields, upload_field, path):
    boundary = "fugue" + secrets.token_hex(16)
    parts = []
    for name, value in fields.items():
        parts.append(
            ('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n' % (boundary, quote_field(name))).encode()
            + value.encode("utf-8")
            + b"\r\n"
        )
    filename = os.path.basename(path)
    kind = mimetypes.guess_type(filename)[0] or "application/octet-stream"
    with open(path, "rb") as handle:
        content = handle.read()
    parts.append(
        (
            '--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\nContent-Type: %s\r\n\r\n'
            % (boundary, quote_field(upload_field), quote_field(filename), kind)
        ).encode()
        + content
        + b"\r\n"
    )
    parts.append(("--%s--\r\n" % boundary).encode())
    return b"".join(parts), "multipart/form-data; boundary=" + boundary


def quote_field(text):
    return text.replace("\\", "\\\\").replace('"', "%22").replace("\r", "").replace("\n", "")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def call(method, url, headers, data, timeout, limit):
    parsed = urllib.parse.urlsplit(url)
    host = parsed.hostname or ""
    loopback = is_loopback(host)
    where = host + (":%d" % parsed.port if parsed.port else "")
    handlers = [NoRedirect()]
    if loopback:
        handlers.append(urllib.request.ProxyHandler({}))
    opener = urllib.request.build_opener(*handlers)
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    deadline = time.monotonic() + timeout
    try:
        try:
            response = opener.open(request, timeout=timeout)
        except urllib.error.HTTPError as error:
            response = error  # an HTTPError is a response too: its status and body are outputs
        with response:
            declared = response.headers.get("Content-Length")
            if declared and declared.isdigit() and int(declared) > limit:
                fail("response_too_large", "The answer from %s is %s bytes; the limit is %d." % (where, declared, limit),
                     "Raise max_response_mb, or ask for less.")
            chunks, size = [], 0
            while True:
                if time.monotonic() > deadline:
                    raise socket.timeout()
                chunk = response.read(65536)
                if not chunk:
                    break
                size += len(chunk)
                if size > limit:
                    fail("response_too_large", "The answer from %s is over the limit of %d bytes." % (where, limit),
                         "Raise max_response_mb, or ask for less.")
                chunks.append(chunk)
            return response.status, b"".join(chunks)
    except (socket.timeout, TimeoutError):
        fail("node_timeout", "%s did not answer within %d seconds." % (where, timeout),
             "Raise timeout_s, or check that the service is up.")
    except urllib.error.URLError as error:
        reason = error.reason
        if isinstance(reason, (socket.timeout, TimeoutError)):
            fail("node_timeout", "%s did not answer within %d seconds." % (where, timeout),
                 "Raise timeout_s, or check that the service is up.")
        fail("network_error", "Could not reach %s: %s." % (where, reason), "Check the address and the connection.")
    except (OSError, ValueError) as error:
        fail("network_error", "The call to %s failed: %s." % (where, error), "Check the address and the connection.")


PATH_TOKEN = re.compile(r"""\.?([A-Za-z0-9_\-]+)|\[(-?\d+)\]|\[\"((?:[^\"\\]|\\.)*)\"\]|\[\'((?:[^\'\\]|\\.)*)\'\]""")


def parse_path(path):
    """data.items[0].name, $.a["b c"], items[-1]: keys, [index] and ["key"]. No wildcards, filters or slices."""
    text = path.strip()
    if text.startswith("$"):
        text = text[1:]
    steps, position = [], 0
    while position < len(text):
        match = PATH_TOKEN.match(text, position)
        if not match or match.end() == position:
            fail("bad_input", "The path %r is not valid at character %d." % (path, position + 1),
                 "Use a form like data.items[0].name.")
        if match.group(1) is not None:
            steps.append(match.group(1))
        elif match.group(2) is not None:
            steps.append(int(match.group(2)))
        else:
            steps.append((match.group(3) if match.group(3) is not None else match.group(4)).replace("\\", ""))
        position = match.end()
    return steps


def pick(document, steps):
    """The value at the path, or (False, None) when any step is missing."""
    node = document
    for step in steps:
        if isinstance(step, int) and isinstance(node, list) and -len(node) <= step < len(node):
            node = node[step]
        elif isinstance(step, str) and isinstance(node, dict) and step in node:
            node = node[step]
        elif isinstance(step, str) and isinstance(node, list) and step.lstrip("-").isdigit() and -len(node) <= int(step) < len(node):
            node = node[int(step)]  # items.0 is items[0]
        else:
            return False, None
    return True, node


def as_text(value):
    if isinstance(value, str):
        return value
    if isinstance(value, bool):
        return "true" if value else "false"
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def run(inputs, secrets):
    values = {name: str(inputs.get(name) or "") for name in "abcd"}
    method = str(inputs.get("method") or "GET").upper()
    if method not in METHODS:
        fail("bad_input", "The method %r is not one of %s." % (method[:20], ", ".join(METHODS)))
    template_url = str(inputs.get("url") or "").strip()
    header_text = str(inputs.get("headers") or "")
    body_kind = str(inputs.get("body_kind") or "none")
    body_text = str(inputs.get("body") or "")
    timeout = max(1, int(inputs.get("timeout_s") or 30))
    limit = max(1, int(inputs.get("max_response_mb") or 5)) * 1024 * 1024
    upload_field = str(inputs.get("upload_field") or "file")

    check_no_secret(body_text, "the body")
    for where, text in (("the URL", template_url), ("a header", header_text)):
        check_secret_names(text, where)
    if not template_url:
        fail("bad_input", "The URL is empty.", "Set url, for example https://example.com/api.")

    secrets.request_origin = template_origin(template_url)  # from the written-out address, before anything is filled
    url = fill_text(template_url, values, quote=True, secrets=secrets)
    parsed = urllib.parse.urlsplit(url)
    host = parsed.hostname or ""
    if parsed.scheme not in ("http", "https") or not host:
        fail("bad_input", "The URL must start with https:// and name a host.")
    if parsed.username or parsed.password:
        fail("bad_input", "The URL has a user name or password in it.", "Put credentials in a header instead.")
    if parsed.scheme == "http" and not is_loopback(host):
        fail("bad_input", "Only https is allowed; %s is not this machine." % host,
             "Use https://. Plain http works only for localhost, 127.0.0.1 and ::1.")

    headers = parse_headers(header_text, values, secrets)

    data, kind, fields = None, None, None
    if body_kind == "json":
        try:
            template = json.loads(body_text)
        except ValueError as error:
            fail("bad_input", "The body is not valid JSON: %s." % error, "Placeholders go inside strings, as \"{a}\".")
        document = fill_json(template, values)
        if isinstance(document, dict):
            fields = {key: item if isinstance(item, str) else as_text(item) for key, item in document.items()}
        data = json.dumps(document, ensure_ascii=False).encode("utf-8")
        kind = "application/json"
    elif body_kind == "text":
        text = fill_text(body_text, values)
        data = text.encode("utf-8")
        fields = {"body": text}
        kind = "text/plain; charset=utf-8"

    upload = None
    for name in UPLOADS:
        handle = inputs.get(name)
        if isinstance(handle, dict) and handle.get("path"):
            upload = handle["path"]
            break
    if upload:
        try:
            data, kind = multipart(fields or {}, upload_field, upload)
        except OSError as error:
            fail("bad_input", "The upload cannot be read: %s." % error.strerror)
    if data is not None and not has_header(headers, "content-type"):
        headers["Content-Type"] = kind
    if not has_header(headers, "user-agent"):
        headers["User-Agent"] = "Fugue-HTTP-Request/1.0"

    status, raw = call(method, url, headers, data, timeout, limit)
    text = raw.decode("utf-8", errors="replace")
    if status >= 400 and inputs.get("fail_on_error_status"):
        fail("http_status", "%s answered %d." % (parsed.hostname, status),
             "Turn off fail_on_error_status to get the status and body as outputs.")

    outputs = {"status": status, "body": text}
    paths = [str(inputs.get("path%d" % i) or "").strip() for i in (1, 2, 3)]
    document, parsed_json = None, False
    for index, path in enumerate(paths, start=1):
        if not path:
            continue
        steps = parse_path(path)
        if not parsed_json:
            parsed_json = True
            try:
                document = json.loads(text)
            except ValueError:
                document = None
        found, value = pick(document, steps) if document is not None else (False, None)
        if found and value is not None:
            outputs["field%d" % index] = as_text(value)
    return outputs


def main():
    secrets = Secrets(None)
    try:
        request = json.loads(sys.stdin.read())
        secrets = Secrets(request.get("secrets"))
        outputs = run(request.get("inputs", {}), secrets)
        line = {"op": "result", "outputs": outputs}
    except Failure as failure:
        line = {"op": "error", "code": failure.code, "message": failure.text}
    line = secrets.mask_all(line)  # no value of a secret leaves the node: not in an output, not in an error
    print(json.dumps(line, ensure_ascii=False))
    sys.stdout.flush()
    return 0 if line["op"] == "result" else 1


if __name__ == "__main__":
    sys.exit(main())
