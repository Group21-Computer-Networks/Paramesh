// The playground page. It shows what the server returns: nothing here makes up a figure, and
// whatever the server has no number for reads "Not available".
const $ = (id) => document.getElementById(id);
const NA = "Not available";
const examples = window.PM_EXAMPLES;
const API = ["pm_init", "pm_finalize", "pm_malloc", "pm_parallel_for", "pm_parallel_for_data", "pm_wait_all",
  "pm_atomic_add", "pm_lock", "pm_unlock", "pm_barrier_wait", "pm_touch", "pm_strerror", "PM_TASK", "PM_OK",
  "pm_config", "pm_task_ctx", "PM_PAGE_SIZE"];

let editor = null;
let monacoApi = null;
let current = null;  // the run being watched
let last = null;     // the last run's whole answer
let dark = false;

const code = () => (editor ? editor.getValue() : $("fallback").value);
const setCode = (text) => { if (editor) editor.setValue(text); else $("fallback").value = text; };

function html(tag, attributes, ...children) {
  const node = document.createElement(tag);
  Object.assign(node, attributes);
  node.append(...children);
  return node;
}

// ---- the editor ------------------------------------------------------------------------------

function fallbackEditor() {
  const area = html("textarea", { id: "fallback", spellcheck: false, ariaLabel: "C program" });
  area.addEventListener("keydown", (event) => {
    if (event.key !== "Tab") return;
    event.preventDefault();
    area.setRangeText("    ", area.selectionStart, area.selectionEnd, "end");
  });
  $("editor").replaceChildren(area);
  area.value = examples[3].source;
}

function startEditor() {
  const base = "https://cdn.jsdelivr.net/npm/monaco-editor@0.52.2/min/vs";
  const script = html("script", { src: `${base}/loader.js` });
  script.onerror = fallbackEditor;  // offline: a plain text box still runs programs
  script.onload = () => {
    window.require.config({ paths: { vs: base } });
    window.require(["vs/editor/editor.main"], (api) => {
      monacoApi = api;
      api.editor.defineTheme("paramesh-paper", { base: "vs", inherit: true, rules: [
        { token: "comment", foreground: "6a7588" }, { token: "keyword", foreground: "47698f" },
        { token: "string", foreground: "8a6d3f" }, { token: "number", foreground: "2e6b58" }],
        colors: { "editor.background": "#fbf8f1", "editorLineNumber.foreground": "#a7a294", "editor.lineHighlightBackground": "#f2ecdf" } });
      api.editor.defineTheme("paramesh-night", { base: "vs-dark", inherit: true, rules: [
        { token: "comment", foreground: "8795a8" }, { token: "keyword", foreground: "9dc2de" },
        { token: "string", foreground: "dcc497" }],
        colors: { "editor.background": "#14233f", "editor.lineHighlightBackground": "#1c2e50" } });
      editor = api.editor.create($("editor"), { value: examples[3].source, language: "c", theme: "paramesh-paper",
        automaticLayout: true, minimap: { enabled: false }, fontFamily: "IBM Plex Mono, monospace", fontSize: 14,
        tabSize: 4, insertSpaces: true, autoIndent: "full", matchBrackets: "always", scrollBeyondLastLine: false });
      api.languages.registerCompletionItemProvider("c", { provideCompletionItems: (model, position) => {
        const word = model.getWordUntilPosition(position);
        const range = { startLineNumber: position.lineNumber, endLineNumber: position.lineNumber,
          startColumn: word.startColumn, endColumn: word.endColumn };
        return { suggestions: API.map((label) => ({ label, insertText: label, range,
          kind: api.languages.CompletionItemKind.Function })) };
      } });
    }, fallbackEditor);
  };
  document.head.append(script);
}

function markErrors(diagnostics) {
  if (!monacoApi) return;
  monacoApi.editor.setModelMarkers(editor.getModel(), "gcc", (diagnostics || []).map((d) => ({
    startLineNumber: d.line, endLineNumber: d.line, startColumn: d.column, endColumn: d.column + 1,
    message: d.message,
    severity: d.severity === "error" ? monacoApi.MarkerSeverity.Error : monacoApi.MarkerSeverity.Warning })));
}

// ---- the pool --------------------------------------------------------------------------------

async function poolStatus() {
  let cluster = { online: false };
  try {
    const answer = await fetch("/api/cluster");
    if (answer.ok) cluster = await answer.json();
  } catch { /* shown below */ }
  $("dot").classList.toggle("on", cluster.online && cluster.member === "joined");
  $("code-field").hidden = !cluster.access_code;
  if (!cluster.online) {
    $("pool").textContent = "pmd is not answering";
  } else if (cluster.member !== "joined") {
    $("pool").textContent = "This node has not joined the pool";
  } else {
    $("pool").textContent = `${cluster.nodes} node${cluster.nodes === 1 ? "" : "s"}, ${cluster.cores} cores`;
  }
  const select = $("nodes");
  const count = Math.max(1, cluster.online ? cluster.nodes : 1);
  if (select.options.length !== count) {
    const kept = select.value;
    select.replaceChildren(...Array.from({ length: count }, (_, i) => html("option", { textContent: i + 1 })));
    select.value = Math.min(kept, count);
  }
  if (!current) $("run").disabled = !(cluster.online && cluster.member === "joined");
}

// ---- running ---------------------------------------------------------------------------------

function showTab(name) {
  for (const tab of ["output", "analytics", "history"]) {
    $(`tab-${tab}`).setAttribute("aria-selected", tab === name);
    $(`pane-${tab}`).hidden = tab !== name;
  }
  if (name === "history") pastRuns();
}

function outputText(run) {
  const parts = [];
  if (run.compiler_output) parts.push(`--- compiler ---\n${run.compiler_output}`);
  if (run.stdout) parts.push(`--- standard output ---\n${run.stdout}`);
  if (run.stderr) parts.push(`--- standard error ---\n${run.stderr}`);
  if (run.worker_output) parts.push(`--- workers ---\n${run.worker_output}`);
  if (run.exit_code !== undefined) parts.push(`--- exit status ${run.exit_code} ---`);
  return parts.join("\n");
}

function stream(title, text, kind) {
  return html("div", { className: `stream ${kind}` }, html("h4", { textContent: title }), html("pre", { textContent: text }));
}

function phases(run) {
  const done = run.state === "done";
  const steps = [["Compile", "compiling"], ["Stage", "staging"], ["Run", "running"]];
  const order = steps.map((s) => s[1]);
  const row = html("div", { className: "phases" });
  steps.forEach(([label, state], index) => {
    const reached = done ? (index === 0 ? "compile_ok" in run : index === 1 ? "stage_ms" in run : "run_ms" in run)
      : order.indexOf(run.state) >= index;
    let mark = "";
    if (reached) mark = !done && run.state === state ? "now" : "ok";
    if (done && index === 0 && run.compile_ok === false) mark = "fail";
    if (done && index === 2 && reached && run.exit_code !== 0) mark = "fail";
    row.append(html("span", { className: mark, textContent: label }));
  });
  return row;
}

function renderOutput(run) {
  const pane = $("pane-output");
  const nodes = [phases(run)];
  if (run.internal_error) nodes.push(html("p", { className: "notice bad", textContent: `The server could not finish this run: ${run.internal_error}` }));
  if (run.refused) nodes.push(html("p", { className: "notice bad", textContent: run.refused }));
  if (run.stopped) nodes.push(html("p", { className: "notice", textContent: "Stopped before the program started." }));
  if (run.timed_out) nodes.push(html("p", { className: "notice bad", textContent: "Stopped: the program went past the time limit." }));
  if (run.compile_ok === false) nodes.push(html("p", { className: "notice bad", textContent: "Compilation failed, so nothing ran." }));
  if (run.compiler_output) nodes.push(stream("Compiler", run.compiler_output, "compiler"));
  const stdout = run.stdout ?? run.live_stdout;
  if (stdout) nodes.push(stream("Standard output", stdout, "stdout"));
  if (run.stderr) nodes.push(stream("Standard error", run.stderr, "stderr"));
  if (run.worker_output) nodes.push(stream("Output from workers", run.worker_output, "stdout"));
  if (run.output_cut) nodes.push(html("p", { className: "notice", textContent: "The output was cut at 256 KiB." }));
  if (run.exit_code !== undefined) {
    nodes.push(html("p", { className: run.exit_code === 0 ? "notice" : "notice bad", textContent: `Exit status ${run.exit_code}${run.exit_code > 128 ? `: ended by signal ${run.exit_code - 128}` : ""}` }));
  }
  pane.replaceChildren(...nodes);
}

const number = (value) => (value === undefined || value === null ? NA : Number(value).toLocaleString());
const ms = (value) => (value === undefined || value === null ? NA : value >= 1000 ? `${(value / 1000).toFixed(2)} s` : `${Math.round(value)} ms`);
const bytes = (value) => {
  if (value === undefined || value === null) return NA;
  return value >= 1048576 ? `${(value / 1048576).toFixed(1)} MiB` : `${(value / 1024).toFixed(1)} KiB`;
};

function kpi(label, value) {
  return html("div", { className: "kpi" }, html("span", { textContent: label }), html("b", { textContent: value }));
}

function bars(rows) {
  const most = Math.max(...rows.map((r) => r.value || 0), 1);
  const grid = html("div", { className: "bars" });
  for (const row of rows) {
    const bar = html("i");
    bar.style.width = `${Math.max(1, (row.value / most) * 100)}%`;
    grid.append(html("span", { textContent: row.label }), html("div", {}, bar), html("span", { textContent: number(row.value) }));
  }
  return grid;
}

function table(groups) {
  const body = html("tbody");
  for (const [group, rows] of groups) {
    body.append(html("tr", {}, html("th", { className: "group", colSpan: 2, textContent: group })));
    for (const [label, value] of rows) {
      body.append(html("tr", {}, html("td", { textContent: label }), html("td", { className: value === NA ? "na" : "", textContent: value })));
    }
  }
  return html("table", {}, body);
}

function renderAnalytics(run) {
  const pane = $("pane-analytics");
  if (run.state !== "done") {
    pane.replaceChildren(html("div", { className: "empty" }, html("h3", { textContent: "Running" }), html("p", { textContent: "The figures arrive when the job ends, because each process reports its own as it exits." })));
    return;
  }
  const m = run.metrics || {};
  const nodes = m.nodes_ran_tasks && m.nodes_ran_tasks.length ? m.nodes_ran_tasks.length : null;
  const cpu = m.cpu_ms ?? run.launcher_cpu_ms;
  const parts = [["Compile", run.compile_ms, "#a98a5c"], ["Stage", run.stage_ms, "#c3d8e8"], ["Program", run.run_ms, "#47698f"]].filter((p) => p[1]);
  const total = parts.reduce((sum, p) => sum + p[1], 0);
  const timeline = html("div", { className: "timeline" });
  const legend = html("div", { className: "legend" });
  for (const [label, value, color] of parts) {
    const piece = html("i");
    piece.style.width = `${(value / total) * 100}%`;
    piece.style.background = color;
    timeline.append(piece);
    const swatch = html("i");
    swatch.style.background = color;
    legend.append(html("span", {}, swatch, `${label} ${ms(value)}`));
  }
  const by = Object.entries(m.by_node || {});
  const content = [
    html("div", { className: "kpis" }, kpi("Compile", ms(run.compile_ms)), kpi("Program wall clock", ms(run.run_ms)),
      kpi("CPU, all processes", ms(cpu)), kpi("Nodes that ran tasks", nodes ?? NA), kpi("Exit status", run.exit_code ?? NA)),
    html("p", { className: "chart-title", textContent: "Execution timeline" }), timeline, legend,
  ];
  if (by.length) {
    content.push(html("p", { className: "chart-title", textContent: "Page faults by node" }), bars(by.map(([node, p]) => ({ label: `Node ${node}`, value: p.faults }))));
    content.push(html("p", { className: "chart-title", textContent: "Pages received by node" }), bars(by.map(([node, p]) => ({ label: `Node ${node}`, value: p.received }))));
  }
  if (m.fault_latency && m.fault_latency.some(Boolean)) {
    const most = Math.max(...m.fault_latency);
    const chart = html("div", { className: "histogram" });
    m.fault_latency.forEach((count, bucket) => {
      const column = html("i", { title: `${count} faults, below ${bucket === 15 ? "over 16,384" : 2 ** bucket} µs` });
      column.style.height = `${(count / most) * 100}%`;
      chart.append(column);
    });
    content.push(html("p", { className: "chart-title", textContent: "Fault latency, power-of-two buckets from 1 µs to over 16 ms" }), chart);
  }
  const chunks = Object.entries(m.chunks_by_peer || {});
  content.push(html("p", { className: "chart-title", textContent: "Every measurement" }), table([
    ["Execution", [["Compile time", ms(run.compile_ms)], ["Staging to peers", ms(run.stage_ms)], ["Program wall clock", ms(run.run_ms)],
      ["CPU time, summed over processes", ms(m.cpu_ms)], ["CPU time, launcher and pmrun", ms(run.launcher_cpu_ms)], ["Exit status", run.exit_code ?? NA]]],
    ["Distribution", [["Processes that reported", number(m.processes_reported)], ["Nodes that ran tasks", nodes ? m.nodes_ran_tasks.join(", ") : NA],
      ["Worker threads on this node", number(m.worker_threads)],
      ...(chunks.length ? chunks.map(([peer, count]) => [`Chunks run by peer ${peer}`, number(count)]) : [["Chunks run by peers", NA]]),
      ["Tasks scheduled", NA], ["Tasks completed", NA]]],
    ["Resources", [["CPU utilisation", NA], ["Peak memory, launcher", run.launcher_peak_kib ? bytes(run.launcher_peak_kib * 1024) : NA], ["Peak memory, workers", NA],
      ["Shared memory allocated", NA], ["Page data sent", bytes(m.page_bytes)], ["Page data if sent plain", bytes(m.page_bytes_plain)]]],
    ["Runtime", [["Page faults", number(m.page_faults)], ["Pages received", number(m.pages_received)], ["Pages sent", number(m.pages_sent)],
      ["Pages prefetched", number(m.pages_prefetched)], ["Requests sent again", number(m.requests_sent_again)], ["Pages sent encoded", number(m.pages_encoded)],
      ["Pages sent as diffs", number(m.pages_as_diffs)], ["Invalidations", NA]]],
  ]));
  pane.replaceChildren(...content);
}

async function pastRuns() {
  const pane = $("pane-history");
  try {
    const { runs } = await (await fetch("/api/runs")).json();
    const finished = runs.filter((r) => r.run_ms).reverse();
    if (!finished.length) {
      pane.replaceChildren(html("div", { className: "empty" }, html("h3", { textContent: "No runs yet" }), html("p", { textContent: "Each run adds a line here, with no source code: its timing, its node count and what the runtime reported." })));
      return;
    }
    const head = html("tr", {}, ...["Program", "Nodes", "Wall clock", "Pages in", "Exit"].map((t) => html("th", { textContent: t })));
    const rows = finished.map((r) => html("tr", {}, html("td", { className: "mono", textContent: r.program }), html("td", { textContent: r.nodes_asked }),
      html("td", { textContent: ms(r.run_ms) }), html("td", { textContent: number(r.metrics?.pages_received) }), html("td", { textContent: r.exit_code })));
    pane.replaceChildren(html("table", {}, html("thead", {}, head), html("tbody", {}, ...rows)));
  } catch {
    pane.replaceChildren(html("p", { className: "notice bad", textContent: "Could not load past runs." }));
  }
}

function running(active) {
  $("run").disabled = active;
  $("stop").disabled = !active;
  $("run").textContent = active ? "Running" : "Run Code";
}

async function runProgram() {
  const headers = { "Content-Type": "application/json", "X-Access-Code": $("code").value.trim() };
  showTab("output");
  markErrors([]);
  running(true);
  const empty = { state: "compiling" };
  renderOutput(empty);
  let answer;
  try {
    answer = await fetch("/api/run", { method: "POST", headers, body: JSON.stringify({ language: "c", source: code(), nodes: Number($("nodes").value) }) });
  } catch {
    $("pane-output").replaceChildren(html("p", { className: "notice bad", textContent: "The playground server is not answering." }));
    running(false);
    return;
  }
  const body = await answer.json().catch(() => ({}));
  if (answer.status !== 202) {
    $("pane-output").replaceChildren(html("p", { className: "notice bad", textContent: body.error || "The server refused the run." }));
    running(false);
    return;
  }
  current = body.id;
  while (current) {
    await new Promise((done) => setTimeout(done, 250));
    let state;
    try {
      state = await (await fetch(`/api/run/${current}`)).json();
    } catch {
      continue;
    }
    last = state;
    renderOutput(state);
    renderAnalytics(state);
    if (state.state === "done") {
      markErrors(state.diagnostics);
      current = null;
    }
  }
  running(false);
}

function emptyPanes() {
  $("pane-output").replaceChildren(html("div", { className: "empty" }, html("h3", { textContent: "Nothing has run yet" }), html("p", { textContent: "Press Run Code. Your program is compiled on this node, copied to the nodes you ask for and run by the real ParaMesh runtime, in a sandbox." })));
  $("pane-analytics").replaceChildren(html("div", { className: "empty" }, html("h3", { textContent: "No measurements yet" }), html("p", { textContent: "After a run, every figure here comes from the processes that ran it. Anything the runtime does not report says Not available." })));
}

// ---- controls --------------------------------------------------------------------------------

for (const example of examples) $("examples").append(html("option", { textContent: example.name }));
const chosen = () => examples[$("examples").selectedIndex];
$("examples").selectedIndex = 3;
$("examples").onchange = () => setCode(chosen().source);
$("reset").onclick = () => setCode(chosen().source);
$("clear").onclick = () => setCode("");
$("copy").onclick = () => navigator.clipboard?.writeText(code());
$("run").onclick = runProgram;
$("stop").onclick = () => current && fetch(`/api/stop/${current}`, { method: "POST", headers: { "X-Access-Code": $("code").value.trim() } });
$("theme").onclick = () => {
  dark = !dark;
  $("theme").textContent = dark ? "Light editor" : "Dark editor";
  monacoApi?.editor.setTheme(dark ? "paramesh-night" : "paramesh-paper");
};
$("tab-output").onclick = () => showTab("output");
$("tab-analytics").onclick = () => showTab("analytics");
$("tab-history").onclick = () => showTab("history");
$("clear-output").onclick = () => { last = null; emptyPanes(); };
$("copy-output").onclick = () => last && navigator.clipboard?.writeText(outputText(last));
$("download-output").onclick = () => {
  if (!last) return;
  const link = html("a", { href: URL.createObjectURL(new Blob([outputText(last)], { type: "text/plain" })), download: `paramesh-${last.id}.txt` });
  link.click();
  URL.revokeObjectURL(link.href);
};

emptyPanes();
startEditor();
poolStatus();
setInterval(poolStatus, 4000);
