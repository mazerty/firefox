# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
Support for running mach python-test tasks (via run-task)
"""


from taskgraph.util.schema import Schema
from voluptuous import Any, Optional, Required

from gecko_taskgraph.transforms.job import configure_taskdesc_for_run, run_job_using

python_test_schema = Schema(
    {
        Required("using"): "python-test",
        # Python version to use
        Required("python-version"): int,
        # The subsuite to run
        Required("subsuite"): str,
        # Base work directory used to set up the task.
        Optional("workdir"): str,
        # Use the specified caches.
        Optional("use-caches"): Any(bool, [str]),
        # Prepend the specified ENV variables to the command. This can be useful
        # if the value of the ENV needs to be interpolated with another ENV.
        Optional("prepend-env"): {str: str},
    }
)


defaults = {
    "python-version": 3,
    "subsuite": "default",
}


@run_job_using(
    "docker-worker", "python-test", schema=python_test_schema, defaults=defaults
)
@run_job_using(
    "generic-worker", "python-test", schema=python_test_schema, defaults=defaults
)
def configure_python_test(config, job, taskdesc):
    run = job["run"]
    worker = job["worker"]

    # defer to the mach implementation
    run["mach"] = ("python-test --subsuite {subsuite} --run-slow").format(**run)
    run["using"] = "mach"
    del run["subsuite"]
    configure_taskdesc_for_run(config, job, taskdesc, worker["implementation"])
