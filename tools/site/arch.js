// The architecture section: choose a part to read about it, or play a story through the parts.
{
const calm = matchMedia("(prefers-reduced-motion: reduce)").matches;
const $ = (id) => document.getElementById(id);

const PARTS = {
  app: ["Your program", "The same executable runs on every node. main() runs only on the launcher; workers run its tasks."],
  api: ["paramesh.h", "The C API: pm_init, pm_malloc, PM_TASK, pm_parallel_for, pm_wait_all, locks and barriers."],
  launcher: ["Launcher process", "Runs main(), holds the task queue, and manages locks and barriers."],
  worker: ["Worker process", "The same program, started by pmd. pm_init never returns here: it serves tasks until the job ends."],
  sched: ["Task scheduler", "Holds the queue of chunks. It prefers to give a node the chunks whose pages that node is home for."],
  threads: ["Worker threads", "Ask the launcher for chunks and run your task on them, as many as the node's quota allows."],
  coh: ["Coherence", "Each page has a home node that tracks who holds it and hands out read or write access. A write first invalidates every other copy."],
  fault: ["Fault handler", "Touching a page this node does not hold raises a fault through userfaultfd. The thread sleeps until the page arrives."],
  pmd: ["pmd", "One daemon on each machine. It finds the other daemons, admits jobs and picks the peers with the most free capacity, each of which may decline. It starts the worker processes."],
  pmrun: ["pmrun", "Starts the launcher and asks the local pmd for N nodes."],
  region: ["Shared-memory region", "The same addresses on every node, in 2 MiB segments, each with a home node. A pointer from pm_malloc means the same thing everywhere."],
};

const AT = {
  pmrun: [290, 374], pmd1: [135, 374], pmd2: [630, 374], app1: [190, 113], app2: [630, 113],
  sched: [190, 203], threads: [630, 203], coh1: [190, 251], coh2: [630, 251], fault1: [190, 299],
  fault2: [630, 299], region: [640, 471], off: null,
};

const STORIES = [
  { name: "Start a job", steps: [
    ["pmrun", ["pmrun"], "You run pmrun", "pmrun -n 2 starts the launcher and asks the local pmd for two nodes."],
    ["pmd1", ["pmd"], "pmd admits the job", "It checks the job against what the pool has free, and picks the peer with the most free capacity."],
    ["pmd2", ["pmd"], "The peer agrees", "Each peer may decline. This one accepts and checks that its copy of the program is the same file."],
    ["app2", ["worker"], "A worker starts", "pmd starts the same program on Node 2. It joins the job inside pm_init."],
    ["app1", ["launcher"], "main() goes on", "Once every node has joined, pm_init returns on the launcher. Workers stay in it, waiting for tasks."],
  ] },
  { name: "Run a task", steps: [
    ["app1", ["app"], "main() queues work", "pm_parallel_for cuts the range into chunks and returns at once."],
    ["sched", ["sched"], "Chunks wait in the queue", "The launcher holds the queue. Any node may take any chunk."],
    ["threads", ["threads"], "A worker asks for a chunk", "Faster nodes come back sooner, so they take more."],
    ["region", ["region", "threads"], "Results go to shared memory", "The task writes through the same pointers main() has."],
    ["app1", ["app"], "pm_wait_all returns", "Every chunk has finished. main() reads the results directly, with no copying in your code."],
  ] },
  { name: "Fetch a page", page: 7, steps: [
    ["threads", ["threads"], "A thread touches a page", "It reads an address on Node 2 that this node does not hold. Nothing in your code changes."],
    ["fault2", ["fault"], "The page fault is caught", "userfaultfd wakes the fault handler. The thread sleeps."],
    ["coh2", ["coh"], "It asks the page's home", "Coherence on Node 2 sends a read request across TCP."],
    ["coh1", ["coh"], "The home answers", "Node 1 records Node 2 as a reader and sends the page."],
    ["coh2", ["coh"], "The page arrives", "The bytes come back over TCP."],
    ["threads", ["threads", "fault"], "The thread wakes", "The page is installed and the same instruction carries on, as if it had always been there."],
  ] },
  { name: "Write a shared page", page: 13, steps: [
    ["threads", ["threads"], "A thread writes", "This node holds the page for reading only, so the write faults."],
    ["coh2", ["coh"], "It asks to write", "A write request goes to the page's home."],
    ["coh1", ["coh"], "The home invalidates", "The home tells every other reader to drop its copy, and waits for each to confirm."],
    ["coh2", ["coh"], "Write access is granted", "Only one node can hold a page for writing, so no one reads a stale value."],
    ["region", ["region"], "The write lands", "The next reader anywhere asks the home and gets the new bytes."],
  ] },
  { name: "Lose a node", steps: [
    ["pmd2", ["pmd"], "A laptop goes silent", "Nodes exchange heartbeats. After 3 s of silence a node counts as lost."],
    ["sched", ["launcher"], "The job ends", "Pages only that node held are gone, so the job stops everywhere and names the node."],
    ["app1", ["app"], "No made-up data", "ParaMesh never fills lost data with zeros. A clean failure beats a wrong answer."],
  ] },
];

const map = $("arch-map");
const panel = $("arch-panel");
const stories = $("arch-stories");
const packet = $("arch-packet");
let story = null;
let step = 0;
let timer = null;

function light(names) {
  for (const part of map.querySelectorAll(".part")) {
    part.classList.toggle("lit", names.includes(part.dataset.part));
    part.classList.toggle("dim", names.length > 0 && !names.includes(part.dataset.part) && !part.classList.contains("frame"));
  }
}

function show(kicker, title, text) {
  $("arch-kicker").textContent = kicker;
  $("arch-title").textContent = title;
  $("arch-text").textContent = text;
}

function stop() {
  clearTimeout(timer);
  timer = null;
  $("arch-play").textContent = "Play";
}

function move(place) {
  const [x, y] = AT[place];
  packet.style.opacity = 1;
  packet.style.transform = `translate(${x}px, ${y}px)`;
}

function draw() {
  const [place, parts, title, text] = story.steps[step];
  move(place);
  light(parts);
  show(`${story.name}, step ${step + 1} of ${story.steps.length}`, title, text);
  for (const tile of map.querySelectorAll(".tile-page")) {
    tile.classList.toggle("hot", Number(tile.dataset.page) === story.page);
  }
  $("arch-back").disabled = step === 0;
  $("arch-next").disabled = step === story.steps.length - 1;
}

function advance() {
  if (step === story.steps.length - 1) {
    stop();
    return;
  }
  step += 1;
  draw();
  timer = setTimeout(advance, 2800);
}

function choose(index) {
  stop();
  story = STORIES[index];
  step = 0;
  for (const [at, button] of [...stories.children].entries()) button.setAttribute("aria-pressed", at === index);
  $("arch-controls").hidden = false;
  draw();
  if (!calm) {
    $("arch-play").textContent = "Pause";
    timer = setTimeout(advance, 2800);
  }
}

function about(name) {
  stop();
  story = null;
  for (const button of stories.children) button.setAttribute("aria-pressed", false);
  $("arch-controls").hidden = true;
  packet.style.opacity = 0;
  for (const tile of map.querySelectorAll(".tile-page")) tile.classList.remove("hot");
  light([name]);
  show("Part", ...PARTS[name]);
}

for (const [index, { name }] of STORIES.entries()) {
  const button = document.createElement("button");
  button.type = "button";
  button.textContent = name;
  button.setAttribute("aria-pressed", false);
  button.addEventListener("click", () => choose(index));
  stories.append(button);
}

for (const part of map.querySelectorAll(".part")) {
  const name = part.dataset.part;
  part.setAttribute("tabindex", 0);
  part.setAttribute("role", "button");
  part.setAttribute("aria-label", PARTS[name][0]);
  part.addEventListener("click", () => about(name));
  part.addEventListener("keydown", (event) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      about(name);
    }
  });
}

$("arch-back").addEventListener("click", () => { stop(); step -= 1; draw(); });
$("arch-next").addEventListener("click", () => { stop(); step += 1; draw(); });
$("arch-play").addEventListener("click", () => {
  if (timer) {
    stop();
    return;
  }
  if (step === story.steps.length - 1) step = -1;
  $("arch-play").textContent = "Pause";
  advance();
});

// Without this script the plain list of parts stays.
document.querySelector(".arch-side dl").hidden = true;
stories.hidden = false;
panel.hidden = false;
show("Start here", "Choose a part, or a story", "Every box is clickable. A story walks through what really happens, one step at a time.");
}
