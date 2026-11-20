// Shared mockup code for docs/canvas-look/*.html; derive.py --write inlines this file.
// Builds nodes and wires from plain data with the spec's geometry, so the pages stay short.
// URL hash: #theme=ethereal&r=8&shot (shot hides the switcher for screenshots).
const LOOK = (() => {
  const THEMES = ["ethereal", "tokyo-night", "rose-pine", "flexoki-light", "white"];
  const LIGHT = { "rose-pine": 1, "flexoki-light": 1, white: 1 };
  const SHAPE = { text: "circle", number: "circle", boolean: "circle", image: "square", video: "square",
                  audio: "square", any: "ring" };
  const shapeOf = (type) => SHAPE[type] || "diamond";
  const colorOf = (type) => type === "any" ? "var(--label)" :
    `var(--port-${["text", "number", "boolean", "image", "video", "audio"].includes(type) ? type : "declared"})`;

  // Lucide-style glyphs (ISC); the app bundles them as an icon font (docs/canvas-look.md, "Glyphs").
  const P = {
    check: '<path d="M20 6 9 17l-5-5"/>',
    stale: '<path d="M3 12a9 9 0 0 1 15-6.7L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-15 6.7L3 16"/><path d="M3 21v-5h5"/>',
    spinner: '<path d="M21 12a9 9 0 1 1-6.2-8.6"/>',
    frozen: '<path d="M12 2v20M3.3 7l17.4 10M3.3 17 20.7 7"/><path d="m9 4 3 2 3-2M9 20l3-2 3 2"/>',
    error: '<path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/><path d="M12 9v4M12 17h.01"/>',
    bypass: '<path d="M4 4v7a4 4 0 0 0 4 4h12"/><path d="m15 10 5 5-5 5"/>',
    mute: '<circle cx="12" cy="12" r="9"/><path d="m5.6 5.6 12.8 12.8"/>',
    skip: '<path d="M5 4l10 8-10 8z"/><path d="M19 5v14"/>',
    pin: '<path d="M12 17v5"/><path d="M9 10.8a2 2 0 0 1-1.1 1.8l-1.8.9A2 2 0 0 0 5 15.2V17h14v-1.8a2 2 0 0 0-1.1-1.8l-1.8-.9A2 2 0 0 1 15 10.8V6h1a2 2 0 0 0 0-4H8a2 2 0 0 0 0 4h1z"/>',
    group: '<path d="M12 2 2 7l10 5 10-5z"/><path d="m2 17 10 5 10-5M2 12l10 5 10-5"/>',
    missing: '<circle cx="12" cy="12" r="9"/><path d="M9.1 9a3 3 0 0 1 5.8 1c0 2-3 3-3 3M12 17h.01"/>',
    seed: '<rect x="3" y="3" width="18" height="18" rx="3"/><path d="M8 8h.01M16 8h.01M12 12h.01M8 16h.01M16 16h.01"/>',
    choice: '<path d="m6 9 6 6 6-6"/>',
    folder: '<path d="M4 20h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.7-.9l-.8-1.2A2 2 0 0 0 7.9 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2z"/>',
    model: '<path d="M21 8 12 3 3 8v8l9 5 9-5z"/><path d="m3 8 9 5 9-5M12 13v8"/>',
    search: '<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>',
    reason: '<circle cx="12" cy="12" r="9"/><path d="m15 9-6 6M9 9l6 6"/>',
    warn: '<path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/><path d="M12 9v4M12 17h.01"/>',
    color: '<circle cx="12" cy="12" r="9"/>',
  };
  const icon = (name, cls = "") =>
    `<svg class="${cls}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${P[name]}</svg>`;

  // A stand-in for a generated image: a fox in a misty birch wood at dawn, in CSS.
  const scene = (variant = "") => `<div class="scene ${variant}" style="position:absolute;inset:0;background:
      radial-gradient(ellipse 15% 9% at 57% 77%, #d9652b 0 62%, transparent 66%),
      radial-gradient(ellipse 5% 7% at 66% 70%, #e2733a 0 60%, transparent 64%),
      radial-gradient(ellipse 9% 3% at 46% 80%, #c95a24 0 60%, transparent 64%),
      linear-gradient(to top, rgba(232,226,214,.85), rgba(232,226,214,0) 38%),
      linear-gradient(90deg, transparent 12%, rgba(240,236,228,.9) 12% 13.4%, transparent 13.4% 26%,
        rgba(240,236,228,.55) 26% 27%, transparent 27% 79%, rgba(240,236,228,.95) 79% 81%, transparent 81% 90%,
        rgba(240,236,228,.5) 90% 90.8%, transparent 90.8%),
      radial-gradient(circle at 70% 26%, rgba(255,236,200,1) 0 5%, rgba(255,206,160,.6) 13%, transparent 42%),
      linear-gradient(#f2c49e, #b9c3b4 48%, #5e776a);"></div>`;

  function port(row) {
    const sh = shapeOf(row.type);
    const open = row.connected === false && sh !== "ring" ? " open" : "";
    const w = row.widget && row.connected === false ? " w" : "";
    return `<span class="port ${sh}${open}${w}${row.list ? " list" : ""}${row.hot ? " hot" : ""}" data-port="${row.dir}:${row.name}" style="--c:${colorOf(row.type)}"><i></i></span>`;
  }

  function widget(w, label) {
    const v = w.value;
    switch (w.kind) {
      case "spin": return `<div class="well"><span>${label}</span><span class="v">${v}</span></div>`;
      case "slider": return `<div class="well"><span class="fill" style="width:${w.at * 100}%"></span><span>${label}</span><span class="v">${v}</span></div>`;
      case "seed": return `<div class="well"><span>${label}</span><span class="v">${v}</span>${icon("seed")}</div>`;
      case "choice": return `<div class="well"><span>${label}</span><span class="v">${v}</span>${icon("choice")}</div>`;
      case "file": return `<div class="well"><span>${label}</span><span class="v">${v}</span>${icon("folder")}</div>`;
      case "model": return `<div class="well">${icon("model")}<span class="v" style="margin-left:0">${v}</span>${icon("choice")}</div>`;
      case "toggle": return `<div class="well"><span>${label}</span><span class="switch"></span></div>`;
      case "line": return `<div class="well"><span>${label}</span><span class="v">${v}</span></div>`;
      case "color": return `<div class="well"><span>${label}</span><span class="v">${v}</span><span style="width:12px;height:12px;border-radius:2px;background:${w.swatch}"></span></div>`;
      case "multiline": return `<div class="well">${v}</div>`;
    }
    return "";
  }

  function node(n) {
    const el = document.createElement("div");
    el.className = "node " + (n.cls || "");
    el.id = n.id;
    el.style.left = n.x + "px";
    el.style.top = n.y + "px";
    if (n.w) el.style.width = n.w + "px";
    const k = n.kind ? `<span class="mark kind ${shapeOf(n.kind)}" style="--c:${colorOf(n.kind)}"></span>` :
      n.group ? `<span style="width:14px;height:14px;color:var(--label);display:flex">${icon("group")}</span>` : "";
    const meta = n.meta ? `<span class="meta">${n.meta.text ? `<span>${n.meta.text}</span>` : ""}${n.meta.icons ? n.meta.icons.map(i => icon(i, i === "spinner" ? "spin" : "")).join("") : ""}</span>` : "";
    let rows = "";
    for (const r of n.rows || []) {
      if (r.dir === "out") {
        rows += `<div class="row out">${r.value ? `<span class="val">${r.value}</span>` : ""}<span class="lbl">${r.name}</span>${port(r)}</div>`;
      } else {
        const multi = r.widget && r.widget.kind === "multiline" ? " multi" : "";
        const shown = r.widget && !r.connected ? widget(r.widget, r.name) : `<span class="lbl">${r.name}</span>`;
        rows += `<div class="row in${multi}">${port(r)}${shown}</div>`;
      }
    }
    const thumb = n.thumb ? `<div class="thumb ${n.thumb.wide ? "wide" : ""}">${scene()}${n.thumb.chip ? `<span class="chip">${n.thumb.chip}</span>` : ""}</div>` : "";
    const excerpt = n.excerpt ? `<div class="excerpt">${n.excerpt}</div>` : "";
    const progress = n.progress !== undefined ? `<span class="progress"><i style="width:${n.progress * 100}%"></i></span>` : "";
    const err = n.errchip ? `<div class="errchip">${icon("error")}<span>${n.errchip}</span></div>` : "";
    el.innerHTML = `<div class="hd">${k}<span class="title">${n.title}</span>${meta}${progress}</div>` +
      `<div class="bd">${rows}${thumb}${excerpt}${n.foot ? `<div class="row" style="height:20px;font-size:10px">${n.foot}</div>` : ""}</div>${err}`;
    return el;
  }

  // Port center in world coordinates.
  function portPos(world, nodeId, key) {
    const p = world.querySelector(`#${nodeId} [data-port="${key}"]`);
    const wr = world.getBoundingClientRect(), pr = p.getBoundingClientRect();
    const s = world._scale || 1;
    return { x: (pr.left + pr.width / 2 - wr.left) / s, y: (pr.top + pr.height / 2 - wr.top) / s };
  }
  // Cubic with horizontal tangents (docs/canvas-look.md, "Wires").
  function curve(a, b) {
    const ddx = b.x - a.x;
    const dx = ddx >= 24 ? Math.min(Math.max(ddx * 0.5, 24), 160) : 48 + Math.min(Math.abs(ddx), 400) * 0.25;
    return `M${a.x},${a.y} C${a.x + dx},${a.y} ${b.x - dx},${b.y} ${b.x},${b.y}`;
  }
  const NS = "http://www.w3.org/2000/svg";
  function path(svg, d, attrs) {
    const p = document.createElementNS(NS, "path");
    p.setAttribute("d", d);
    p.setAttribute("fill", "none");
    for (const [k, v] of Object.entries(attrs)) p.setAttribute(k, v);
    svg.appendChild(p);
    return p;
  }
  // state: rest | hi (attached to the selection, or hovered) | dim | run | drag | invalid
  function wire(svg, a, b, type, state = "rest", via) {
    const color = colorOf(type);
    const pts = via ? [a, via, b] : [a, b];
    for (let i = 0; i + 1 < pts.length; i++) {
      const d = curve(pts[i], pts[i + 1]);
      if (state === "hi" || state === "drag" || state === "run")
        path(svg, d, { stroke: "var(--canvas)", "stroke-width": 6, "stroke-linecap": "round" });
      const attrs = { stroke: color, "stroke-width": state === "hi" || state === "drag" ? 2.5 : 2, "stroke-linecap": "round",
                      opacity: state === "dim" ? 0.25 : state === "rest" ? 0.8 : 1 };
      if (state === "invalid") Object.assign(attrs, { stroke: "var(--error)", "stroke-dasharray": "6 5", "stroke-width": 2.5 });
      const p = path(svg, d, attrs);
      if (state === "run") {
        const c = document.createElementNS(NS, "circle");
        c.setAttribute("r", 3.5);
        c.setAttribute("fill", color);
        c.setAttribute("stroke", "var(--canvas)");
        c.setAttribute("stroke-width", 2);
        const len = p.getTotalLength(), at = p.getPointAtLength(len * 0.62);
        c.setAttribute("cx", at.x);
        c.setAttribute("cy", at.y);
        if (!document.body.classList.contains("shot")) {
          const m = document.createElementNS(NS, "animateMotion");
          m.setAttribute("dur", "900ms");
          m.setAttribute("repeatCount", "indefinite");
          m.setAttribute("path", d);
          c.setAttribute("cx", 0); c.setAttribute("cy", 0);
          c.appendChild(m);
        }
        svg.appendChild(c);
      }
    }
  }
  function link(world, svg, from, to, type, state, via) {
    wire(svg, portPos(world, from[0], "out:" + from[1]), portPos(world, to[0], "in:" + to[1]), type, state, via);
  }

  function applyHash() {
    const h = new URLSearchParams(location.hash.slice(1));
    const theme = THEMES.includes(h.get("theme")) ? h.get("theme") : "ethereal";
    const r = h.has("r") ? parseInt(h.get("r"), 10) : 8; // the corner_radius setting (#63)
    document.documentElement.dataset.theme = theme;
    document.documentElement.dataset.mode = LIGHT[theme] ? "light" : "dark";
    document.documentElement.style.setProperty("--r", r + "px");
    document.documentElement.style.setProperty("--R", r + "px");
    document.body.classList.toggle("shot", h.has("shot"));
    return { theme, r };
  }
  function switcher(onChange) {
    const s = applyHash();
    const el = document.createElement("div");
    el.className = "switcher";
    el.innerHTML = `<select id="th">${THEMES.map(t => `<option ${t === s.theme ? "selected" : ""}>${t}</option>`).join("")}</select>` +
      `<select id="rd">${[0, 4, 8, 12, 16].map(v => `<option value="${v}" ${v === s.r ? "selected" : ""}>radius ${v}</option>`).join("")}</select>`;
    document.body.appendChild(el);
    const go = () => { location.hash = `theme=${el.querySelector("#th").value}&r=${el.querySelector("#rd").value}`; };
    el.querySelector("#th").onchange = go;
    el.querySelector("#rd").onchange = go;
    addEventListener("hashchange", () => { applyHash(); onChange && onChange(); });
  }
  return { node, link, wire, portPos, curve, icon, scene, shapeOf, colorOf, switcher, applyHash };
})();
