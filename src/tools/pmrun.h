// pmrun: starts one program as a ParaMesh job. docs/PROTOCOL.md, section 8.1, steps 1 and 2.
#ifndef PARAMESH_TOOLS_PMRUN_H
#define PARAMESH_TOOLS_PMRUN_H

#include <span>
#include <string_view>

namespace paramesh {

// `pmrun -n N [--state-dir DIRECTORY] program [arguments...]`, without the program name.
// Asks the local pmd for a job, starts the program as the job's launcher, and waits for it.
// Returns the exit status for main(): the launcher's own; 130 if interrupted; 1 if the job
// could not be started; 2 for a bad command line.
int pmrun(std::span<const std::string_view> args);

}  // namespace paramesh

#endif  // PARAMESH_TOOLS_PMRUN_H
