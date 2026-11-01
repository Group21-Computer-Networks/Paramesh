# ParaMesh

ParaMesh lets a few ordinary Linux laptops on a LAN run one C program together. The program calls
`paramesh.h`; page faults keep its shared memory coherent across the machines, with no socket
code in the application. Built by Group 21 as a computer networks project.

## Audience

- People who have never heard of distributed shared memory: exhibition visitors, students.
- Researchers and collaborators judging whether the system is real.
- The group itself, demonstrating on two laptops and collecting measurements.

## Job

Understand what ParaMesh does in a minute, then run a real C program on the real pool and see
what the machines did.

## Facts to protect

- Everything shown as live or measured comes from the running system. What is not measured says
  "Not available". Nothing is simulated, estimated or staged.
- The components are: the application, `paramesh.h`, the launcher, `pmd`, worker processes, the
  shared region, the task scheduler, and the MSI-style coherence protocol. Invent no others.
- Fault tolerance is out of scope: a lost node ends the job.
- The playground is for a supervised LAN, with an access code. It is not safe on the internet.

## Platform

Web, plain HTML, CSS and JavaScript with no build step, served by a Python standard-library
server on each node. The dashboard on port 47002 stays as it is.

## Voice

Plain English, short sentences, precise. No hype words.
