// The page grid in the hero, and the live figures, from /api/cluster and nothing else.
const pages = document.getElementById("pages");
const tints = ["#c3d8e8", "#47698f", "#e3d5bc", "#2a3d60"];
for (let row = 0; row < 5; row++) {
  for (let column = 0; column < 8; column++) {
    const cell = document.createElementNS("http://www.w3.org/2000/svg", "rect");
    cell.setAttribute("x", 166 + column * 29);
    cell.setAttribute("y", 166 + row * 29);
    cell.setAttribute("width", 22);
    cell.setAttribute("height", 22);
    cell.setAttribute("rx", 4);
    cell.setAttribute("fill", tints[(row * 3 + column * 5 + (row * column) % 3) % 4]);
    pages.append(cell);
  }
}

const shown = (id, value) => { document.getElementById(id).textContent = value; };

async function refresh() {
  let cluster = { online: false };
  try {
    const answer = await fetch("/api/cluster");
    if (answer.ok) cluster = await answer.json();
  } catch { /* offline, below */ }
  document.getElementById("dot").classList.toggle("on", cluster.online);
  if (!cluster.online) {
    shown("state", "This node's daemon is not answering, so there is nothing to show");
    for (const id of ["c-nodes", "c-cores", "c-free", "c-jobs"]) shown(id, "–");
    return;
  }
  shown("state", cluster.member === "joined" ? `Live from node ${cluster.node}` : `Node ${cluster.node} has not joined the pool`);
  shown("c-nodes", cluster.nodes);
  shown("c-cores", cluster.cores);
  shown("c-free", cluster.free_cores);
  shown("c-jobs", cluster.jobs);
}
refresh();
setInterval(refresh, 3000);
