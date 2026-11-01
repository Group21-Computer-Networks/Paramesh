// The page grid in the hero, and the live figures, from /api/cluster and nothing else.
const pages = document.getElementById("pages");
const tints = ["#c8d8e2", "#3f6386", "#e6d9c1", "#34506f"];
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

const calm = matchMedia("(prefers-reduced-motion: reduce)").matches;

// A live figure runs up to its value the first time, and changes plainly after that.
const counted = new Set();
function count(id, value) {
  if (calm || counted.has(id) || !value) {
    counted.add(id);
    shown(id, value);
    return;
  }
  counted.add(id);
  const begin = performance.now();
  const step = (now) => {
    const done = Math.min(1, (now - begin) / 700);
    shown(id, Math.round(value * (1 - (1 - done) ** 3)));
    if (done < 1) requestAnimationFrame(step);
  };
  requestAnimationFrame(step);
}

// The code on the laptops types itself, line after line, a laptop at a time.
function typeOut() {
  if (calm) return;
  const lines = [...document.querySelectorAll("text.type")];
  const words = lines.map((line) => line.textContent);
  lines.forEach((line) => { line.textContent = ""; });
  let at = 0;
  let letter = 0;
  const next = () => {
    if (at === lines.length) return;
    letter += 1;
    lines[at].textContent = words[at].slice(0, letter);
    const finished = letter === words[at].length;
    if (finished) {
      at += 1;
      letter = 0;
    }
    setTimeout(next, finished ? 240 : 34);
  };
  setTimeout(next, 500);
}
typeOut();

// What comes into view rises into place. Without this script everything simply shows.
if (!calm && "IntersectionObserver" in window) {
  document.documentElement.classList.add("moves");
  const watcher = new IntersectionObserver((seen) => {
    for (const entry of seen) {
      if (entry.isIntersecting) {
        entry.target.classList.add("in");
        watcher.unobserve(entry.target);
      }
    }
  }, { threshold: 0.15 });
  document.querySelectorAll(".tile, .step, .arch, .live").forEach((item, index) => {
    item.classList.add("reveal");
    item.style.setProperty("--wait", `${(index % 3) * 90}ms`);
    watcher.observe(item);
  });
}

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
  count("c-nodes", cluster.nodes);
  count("c-cores", cluster.cores);
  count("c-free", cluster.free_cores);
  count("c-jobs", cluster.jobs);
}
refresh();
setInterval(refresh, 3000);
