# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
Support for running mach tasks (via run-task)
"""

from taskgraph.util.schema import Schema, taskref_or_string
from voluptuous import Any, Optional, Required

from gecko_taskgraph.transforms.job import configure_taskdesc_for_run, run_job_using

mach_schema = Schema(
    {
        Required("using"): "mach",
        # The mach command (omitting `./mach`) to run
        Required("mach"): taskref_or_string,
        # The version of Python to run with. Either an absolute path to the binary
        # on the worker, a version identifier (e.g python2.7 or 3.8). There is no
        # validation performed to ensure the specified binaries actually exist.
        Optional("python-version"): Any(str, int, float),
        # The sparse checkout profile to use. Value is the filename relative to the
        # directory where sparse profiles are defined (build/sparse-profiles/).
        Optional("sparse-profile"): Any(str, None),
        # if true, perform a checkout of a comm-central based branch inside the
        # gecko checkout
        Required("comm-checkout"): bool,
        # Prepend the specified ENV variables to the command. This can be useful
        # if the value of the ENV needs to be interpolated with another ENV.
        Optional("prepend-env"): {str: str},
        # Base work directory used to set up the task.
        Optional("workdir"): str,
        # Use the specified caches.
        Optional("use-caches"): Any(bool, [str]),
    }
)


defaults = {
    "comm-checkout": False,
}


@run_job_using("docker-worker", "mach", schema=mach_schema, defaults=defaults)
@run_job_using("generic-worker", "mach", schema=mach_schema, defaults=defaults)
def configure_mach(config, job, taskdesc):
    run = job["run"]
    worker = job["worker"]

    additional_prefix = []
    if worker["os"] == "macosx":
        additional_prefix = ["LC_ALL=en_US.UTF-8", "LANG=en_US.UTF-8"]

    if prepend_env := run.pop("prepend-env", None):
        for name, value in prepend_env.items():
            additional_prefix.append(f"{name}={value}")

    if python := run.pop("python-version", None):
        if taskdesc.get("use-python", "system") == "system":
            if worker["os"] == "macosx" and python == 3:
                python = "/usr/local/bin/python3"

        python = str(python)
        try:
            float(python)
            python = "python" + python
        except ValueError:
            pass

        additional_prefix.append(python)

    command_prefix = " ".join(additional_prefix + ["./mach "])

    mach = run["mach"]
    if isinstance(mach, dict):
        ref, pattern = next(iter(mach.items()))
        command = {ref: command_prefix + pattern}
    else:
        command = command_prefix + mach

    # defer to the run_task implementation
    run["command"] = command
    run["cwd"] = "{checkout}"
    run["using"] = "run-task"
    del run["mach"]
    configure_taskdesc_for_run(config, job, taskdesc, job["worker"]["implementation"])
