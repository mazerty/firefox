# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
import argparse
import os
import shutil

from run_operations import run_git, run_shell

# This script fetches the libwebrtc repro with the expected
# upstream remote and branch-heads setup.  This is used by both the
# prep_repo.sh script as well as the restore_patch_stack.py script.
#
# For speed and conservation of network resources, after fetching all
# the data, a tar of the repo is made and used if available.


def fetch_repo(github_path, force_fetch, tar_path):
    capture_output = False

    # check for pre-existing repo - make sure we force the removal
    if force_fetch and os.path.exists(github_path):
        print(f"Removing existing repo: {github_path}")
        shutil.rmtree(github_path)

    # clone https://webrtc.googlesource.com/src
    if not os.path.exists(github_path):
        # check for pre-existing tar, use it if we have it
        if os.path.exists(tar_path):
            print("Using tar file to reconstitute repo")
            cmd = f"cd {os.path.dirname(github_path)} ; tar --extract --gunzip --file={os.path.basename(tar_path)}"
            run_shell(cmd, capture_output)
        else:
            print("Cloning github repo")
            run_shell(
                f"git clone https://webrtc.googlesource.com/src {github_path}",
                capture_output,
            )

    # for sanity, ensure we're on master
    run_git("git checkout master", github_path)

    # setup upstream branch-heads
    stdout_lines = run_git(
        "git config --local --get-all remote.origin.fetch", github_path
    )
    if len(stdout_lines) == 1:
        print("Fetching upstream branch-heads")
        run_git(
            "git config --local --add remote.origin.fetch +refs/branch-heads/*:refs/remotes/branch-heads/*",
            github_path,
        )
        run_git("git fetch", github_path)
    else:
        print("Upstream remote branch-heads already configured")

    # verify that a (quite old) branch-head exists
    run_git("git show branch-heads/5059", github_path)

    # prevent changing line endings when moving things out of the git repo
    # (and into hg for instance)
    run_git("git config --local core.autocrlf false", github_path)

    # do a sanity fetch in case this was not a freshly cloned copy of the
    # repo.
    run_git("git fetch --all", github_path)

    # create tar to avoid time refetching
    if not os.path.exists(tar_path):
        print("Creating tar file for quicker restore")
        cmd = f"cd {os.path.dirname(github_path)} ; tar --create --gzip --file={os.path.basename(tar_path)} {os.path.basename(github_path)}"
        run_shell(cmd, capture_output)


if __name__ == "__main__":
    default_state_dir = ".moz-fast-forward"
    default_tar_name = "moz-libwebrtc.tar.gz"

    parser = argparse.ArgumentParser(
        description="Restore moz-libwebrtc github patch stack"
    )
    parser.add_argument(
        "--repo-path",
        required=True,
        help="path to libwebrtc repo",
    )
    parser.add_argument(
        "--force-fetch",
        action="store_true",
        default=False,
        help="force rebuild an existing repo directory",
    )
    parser.add_argument(
        "--tar-name",
        default=default_tar_name,
        help=f"name of tar file (defaults to {default_tar_name})",
    )
    parser.add_argument(
        "--state-path",
        default=default_state_dir,
        help=f"path to state directory (defaults to {default_state_dir})",
    )
    args = parser.parse_args()

    fetch_repo(
        args.repo_path,
        args.force_fetch,
        os.path.join(args.state_path, args.tar_name),
    )
