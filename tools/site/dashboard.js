// The dashboard, from /api/node: what this node's pmd reports, redrawn once a second. It works out
// the same sums as tools/dashboard/index.html, and makes up nothing.
const $ = (id) => document.getElementById(id);
const fixed = (value, digits) => Number(value || 0).toFixed(digits);
const leaving = {};  // peer -> the segments homed on it when it began to leave
let formFilled = false;

function cell(value, numeric) {
  const td = document.createElement("td");
  td.textContent = value;
  if (numeric) td.className = "n";
  return td;
}

function rows(body, lines, empty, columns) {
  body.replaceChildren(...lines.map((line) => {
    const row = document.createElement("tr");
    row.append(...line);
    return row;
  }));
  if (!lines.length) {
    const row = document.createElement("tr");
    const td = cell(empty);
    td.colSpan = columns;
    td.className = "soft";
    row.append(td);
    body.append(row);
  }
}

function drawMember(p, canManage) {
  $("title").textContent = `Node ${p.node}, ${p.cap_cores} cores`;
  $("member").textContent = p.member;
  $("member").className = `badge ${p.member}`;
  $("note").textContent = p.note || (p.member === "standby" ? "Not in the pool: nothing runs here for others." : "");
  $("join").hidden = p.member !== "standby" || !canManage;
  $("joined").hidden = p.member === "standby";
  $("leave").hidden = !canManage;
  $("visitor").hidden = canManage;
  if ($("leave").textContent !== "Leaving") $("leave").disabled = p.member !== "joined";
  $("giving").textContent = `Giving ${p.cap_cores} of ${p.machine_cores} cores and ${fixed(p.cap_ram / 1048576, 0)} of ${fixed(p.machine_ram / 1048576, 0)} MB of RAM.`;
  const cores = $("cores");
  const ram = $("ram");
  cores.max = p.machine_cores;
  ram.max = Math.floor(p.machine_ram / 1048576);
  $("ram-max").textContent = ram.max;
  if (!formFilled) {  // half of each, as a start
    cores.value = Math.max(1, Math.floor(p.machine_cores / 2));
    ram.value = Math.floor(ram.max / 2);
    formFilled = true;
  }
  $("cores-out").textContent = `${cores.value} of ${p.machine_cores}`;
  $("heard").replaceChildren(...(p.peers || []).map((q) => {
    const option = document.createElement("option");
    option.value = q.address;
    option.textContent = `node ${q.node}`;
    return option;
  }));
}

// Credits are in core-seconds. For a job, credit_given is what the peer gave this node, and
// credit_taken what this node gave the peer.
function drawLedger(ledger) {
  let given = 0;
  let received = 0;
  const lines = (ledger.peers || []).map((q) => {
    let gave = 0;
    let got = 0;
    for (const job of q.jobs || []) {
      gave += job.credit_taken;
      got += job.credit_given;
    }
    given += gave;
    received += got;
    const theirs = q.announced === null ? "not heard" : q.disagree ? "disagrees" : "agrees";
    return [cell(q.peer), cell(fixed(gave, 1), true), cell(fixed(got, 1), true), cell(fixed(q.balance, 1), true),
      cell(fixed(q.weight, 2), true), cell(q.totals.chunks_for_peer, true), cell(q.totals.chunks_by_peer, true), cell(theirs)];
  });
  $("given").textContent = fixed(given, 1);
  $("received").textContent = fixed(received, 1);
  const net = given - received;
  $("net").textContent = given + received === 0 ? "Nothing given or received yet."
    : net >= 0 ? `You have given ${fixed(net, 1)} core-seconds more than you received: other nodes' jobs make room for yours.`
    : `You have received ${fixed(-net, 1)} core-seconds more than you gave.`;
  rows($("ledger"), lines, "Nothing given or received yet.", 8);
}

function drawPeers(p) {
  rows($("peers"), (p.peers || []).map((q) => [cell(q.node), cell(q.address), cell(`${q.state}${q.leaving ? ", leaving" : ""}`),
    cell(q.cap_cores, true), cell(q.jobs, true), cell(`${fixed(q.silent_ms / 1000, 1)} s`, true)]), "No peer heard.", 6);
}

function drawJobs(jobs) {
  rows($("jobs"), (jobs.jobs || []).map((x) => [cell(x.job), cell(x.role), cell(x.launcher, true), cell(x.slots, true),
    cell(fixed(x.faults_per_s, 1), true), cell(fixed(x.mb_in_per_s, 2), true), cell(fixed(x.mb_out_per_s, 2), true),
    cell(x.fault_latency.p50_us, true), cell(x.fault_latency.p99_us, true)]), "No job on this node.", 9);
}

function drawPages(all) {
  const blocks = all.flatMap(({ job, segments }) => {
    const label = document.createElement("p");
    label.textContent = `Job ${job}: ${segments.length} segments`;
    const grid = document.createElement("div");
    grid.className = "cells";
    grid.append(...segments.map((s) => {
      const square = document.createElement("i");
      square.className = s.home_here ? "c-home" : s.modified ? "c-modified" : s.shared ? "c-shared" : "c-empty";
      square.title = `segment ${s.segment}: home ${s.home}, ${s.shared} S, ${s.modified} M`;
      return square;
    }));
    return [label, grid];
  });
  if (!blocks.length) {
    const none = document.createElement("p");
    none.className = "soft";
    none.textContent = "Nothing held.";
    blocks.push(none);
  }
  $("pages").replaceChildren(...blocks);
}

// While a peer leaves, its segments move to other homes, as far as this node sees them.
function drawMigration(peers, all) {
  const homedOn = (node) => all.reduce((n, { segments }) => n + segments.filter((s) => s.home === node).length, 0);
  const now = (peers.peers || []).filter((q) => q.leaving).map((q) => q.node);
  for (const node of Object.keys(leaving)) if (!now.includes(Number(node))) delete leaving[node];
  for (const node of now) if (!(node in leaving)) leaving[node] = homedOn(node);
  $("migration").replaceChildren(...now.flatMap((node) => {
    const from = leaving[node];
    const left = homedOn(node);
    const heading = document.createElement("h2");
    heading.textContent = `Node ${node} is leaving: segments moved ${from - left} of ${from}`;
    const meter = document.createElement("div");
    meter.className = "meter";
    const fill = document.createElement("i");
    fill.style.width = `${(100 * (from === 0 ? 1 : (from - left) / from)).toFixed(0)}%`;
    meter.append(fill);
    return [heading, meter];
  }));
}

async function refresh() {
  let node = { online: false };
  try {
    const answer = await fetch("/api/node", { cache: "no-store" });
    if (answer.ok) node = await answer.json();
  } catch { /* shown below */ }
  $("dot").classList.toggle("on", node.online);
  if (!node.online) {
    $("status").textContent = "This node's daemon is not answering";
    return;
  }
  drawMember(node.peers, node.can_manage);
  drawLedger(node.ledger);
  drawPeers(node.peers);
  drawJobs(node.jobs);
  drawPages(node.pages);
  drawMigration(node.peers, node.pages);
  $("status").textContent = `Updated ${new Date().toLocaleTimeString()}`;
}

async function ask(path, body, button, busy) {
  const idle = button.textContent;
  button.disabled = true;
  button.textContent = busy;
  $("ask-error").hidden = true;
  try {
    const answer = await fetch(path, { method: "POST", headers: { "X-ParaMesh": "1", "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
    if (!answer.ok) {
      $("ask-error").textContent = (await answer.json().catch(() => ({}))).error || `Refused: ${answer.status}`;
      $("ask-error").hidden = false;
    }
  } catch {
    $("ask-error").textContent = "The server is not answering.";
    $("ask-error").hidden = false;
  }
  button.textContent = idle;
  button.disabled = false;
  refresh();
}

$("cores").addEventListener("input", () => { $("cores-out").textContent = `${$("cores").value} of ${$("cores").max}`; });
$("join").addEventListener("submit", (event) => {
  event.preventDefault();
  ask("/api/join", { cap_cores: Number($("cores").value), cap_ram: Number($("ram").value) * 1048576, lead: $("lead").value.trim() }, $("join-button"), "Joining");
});
$("leave").addEventListener("click", () => ask("/api/leave", null, $("leave"), "Leaving"));
refresh();
setInterval(refresh, 1000);
