#! /bin/bash -xe

set -x -e

echo "running as" $(id)

# Detect distribution
. /etc/os-release
if [ "${ID}" == "ubuntu" ]; then
    DISTRIBUTION="Ubuntu"
elif [ "${ID}" == "debian" ]; then
    DISTRIBUTION="Debian"
else
    DISTRIBUTION="Unknown"
fi

# Detect release version if supported
FILE="/etc/lsb-release"
if [ -e $FILE ] ; then
    . /etc/lsb-release
    RELEASE="${DISTRIB_RELEASE}"
else
    RELEASE="unknown"
fi

####
# Taskcluster friendly wrapper for performing fx desktop tests via mozharness.
####

# Inputs, with defaults

: GECKO_PATH                    ${GECKO_PATH}
: MOZHARNESS_PATH               ${MOZHARNESS_PATH}
: MOZHARNESS_URL                ${MOZHARNESS_URL}
: MOZHARNESS_SCRIPT             ${MOZHARNESS_SCRIPT}
: MOZHARNESS_CONFIG             ${MOZHARNESS_CONFIG}
: MOZHARNESS_OPTIONS            ${MOZHARNESS_OPTIONS}
: MOZ_ENABLE_WAYLAND            ${MOZ_ENABLE_WAYLAND}
: NEED_XVFB                     ${NEED_XVFB:=true}
: NEED_WINDOW_MANAGER           ${NEED_WINDOW_MANAGER:=false}
: NEED_PULSEAUDIO               ${NEED_PULSEAUDIO:=false}
: NEED_PIPEWIRE                 ${NEED_PIPEWIRE:=false}
: NEED_COMPIZ                   ${NEED_COPMPIZ:=false}
: START_VNC                     ${START_VNC:=false}
: TASKCLUSTER_INTERACTIVE       ${TASKCLUSTER_INTERACTIVE:=false}
: mozharness args               "${@}"
: WORKING_DIR                   ${WORKING_DIR:=$(pwd)}
: WORKSPACE                     ${WORKSPACE:=${WORKING_DIR%/}/workspace}

set -v
mkdir -p "$WORKSPACE"
cd "$WORKSPACE"

fail() {
    echo # make sure error message is on a new line
    echo "[test-linux.sh:error]" "${@}"
    exit 1
}

# start pulseaudio
maybe_start_pulse() {
    if $NEED_PULSEAUDIO; then
        # call pulseaudio for Ubuntu only
        if [ $DISTRIBUTION == "Ubuntu" ]; then
            pulseaudio --daemonize --log-level=4 --log-time=1 --log-target=stderr --start --fail -vvvvv --exit-idle-time=-1 --cleanup-shm --dump-conf
        fi
    fi
    if $NEED_PIPEWIRE; then
        pw_pids=()
        pipewire &
        pw_pids+=($!)

        SOCKET="$XDG_RUNTIME_DIR/pipewire-0"
        attempts=5
        sleep_time=1
        while [ ! -S "$SOCKET" ] && [ $attempts -gt 0 ]; do
            sleep $sleep_time
            sleep_time=$((sleep_time * 2))
            attempts=$((attempts - 1))
        done
        if [ ! -S "$SOCKET" ]; then
            ps auxf || :
            echo "error: no pipewire socket, retrying the task" >&2
            exit 4
        fi

        wireplumber &
        pw_pids+=($!)
        pipewire-pulse &
        pw_pids+=($!)
    fi
}
cleanup_pipewire() {
    if [ -n "$pw_pids" ] && [ $TASKCLUSTER_INTERACTIVE = false ]; then
        kill "${pw_pids[@]}"
    fi
}

# test required parameters are supplied
if [ -z "${MOZHARNESS_PATH}" -a -z "${MOZHARNESS_URL}" ]; then
    fail "MOZHARNESS_PATH or MOZHARNESS_URL must be defined";
fi

if [[ -z ${MOZHARNESS_SCRIPT} ]]; then fail "MOZHARNESS_SCRIPT is not set"; fi
if [[ -z ${MOZHARNESS_CONFIG} ]]; then fail "MOZHARNESS_CONFIG is not set"; fi

if [ $MOZ_ENABLE_WAYLAND ]; then
  NEED_XVFB=true
  NEED_WINDOW_MANAGER=true
fi

# make sure artifact directories exist
mkdir -p "$WORKSPACE/logs"
mkdir -p "$WORKING_DIR/artifacts/public"
mkdir -p "$WORKSPACE/build/blobber_upload_dir"

cleanup_mutter() {
    local mutter_pids=`ps aux | grep 'mutter --wayland' | grep -v grep | awk '{print $2}'`
    if [ "$mutter_pids" != "" ]; then
        echo "Killing the following Mutter processes: $mutter_pids"
        sudo kill $mutter_pids
    else
        echo "No Mutter processes to kill"
    fi
}

cleanup() {
    local rv=$?
    if $NEED_PIPEWIRE; then
        cleanup_pipewire
    fi
    if [ $MOZ_ENABLE_WAYLAND ]; then
        cleanup_mutter
    fi
    if $NEED_XVFB; then
        cleanup_xvfb
    fi
    exit $rv
}
trap cleanup EXIT INT

# Download mozharness with exponential backoff
# curl already applies exponential backoff, but not for all
# failed cases, apparently, as we keep getting failed downloads
# with 404 code.
download_mozharness() {
    local max_attempts=10
    local timeout=1
    local attempt=0

    echo "Downloading mozharness"

    while [[ $attempt < $max_attempts ]]; do
        if curl --fail -o mozharness.zip --retry 10 -L $MOZHARNESS_URL; then
            rm -rf mozharness
            if unzip -q mozharness.zip -d mozharness; then
                return 0
            fi
            echo "error unzipping mozharness.zip" >&2
        else
            echo "failed to download mozharness zip" >&2
        fi
        echo "Download failed, retrying in $timeout seconds..." >&2
        sleep $timeout
        timeout=$((timeout*2))
        attempt=$((attempt+1))
    done

    fail "Failed to download and unzip mozharness"
}

# Download mozharness if we're told to.
if [ ${MOZHARNESS_URL} ]; then
    download_mozharness
    rm mozharness.zip

    if ! [ -d mozharness ]; then
        fail "mozharness zip did not contain mozharness/"
    fi

    MOZHARNESS_PATH=`pwd`/mozharness
fi

# run XVfb in the background, if necessary
if $NEED_XVFB; then
    # note that this file is not available when run under native-worker
    . $HOME/scripts/xvfb.sh
    start_xvfb '1600x1200x24' 0
fi

if $START_VNC; then
    x11vnc > "$WORKING_DIR/artifacts/public/x11vnc.log" 2>&1 &
fi

if $NEED_WINDOW_MANAGER; then
    # This is read by xsession to select the window manager
    . /etc/lsb-release
    if [ $DISTRIBUTION == "Ubuntu" ]; then
        xsession_args=()
        if [ $RELEASE = "18.04" ]; then
            echo export XDG_CURRENT_DESKTOP=GNOME > $HOME/.xsessionrc
        elif [ $RELEASE = "24.04" ]; then
            # taken from /usr/share/xsessions/ubuntu.desktop
            echo export XDG_CURRENT_DESKTOP=ubuntu:GNOME > $HOME/.xsessionrc
            echo export GNOME_SHELL_SESSION_MODE=ubuntu >> $HOME/.xsessionrc
            xsession_args=("/usr/bin/gnome-session --session=ubuntu")
        fi
        if [ $MOZ_ENABLE_WAYLAND ]; then
            echo export XDG_SESSION_TYPE=wayland >> $HOME/.xsessionrc
        else
            echo export XDG_SESSION_TYPE=x11 >> $HOME/.xsessionrc
        fi
    else
        :
    fi
    export XDG_RUNTIME_DIR=$WORKING_DIR

    # Start a session bus early instead of leaving it to Xsession, so that we
    # can use it for access to e.g. gnome-keyring or the screencast API
    if test -z "$DBUS_SESSION_BUS_ADDRESS" ; then
        # if not found, launch a new one
        eval `dbus-launch --sh-syntax`
    fi

    # DISPLAY has already been set above
    # XXX: it would be ideal to add a semaphore logic to make sure that the
    # window manager is ready
    (
        # if env var is >8K, we have a seg fault in xsession
        unset MOZHARNESS_TEST_PATHS
        /etc/X11/Xsession "${xsession_args[@]}" 2>&1 &
    )

    # Turn off the screen saver and screen locking
    gsettings set org.gnome.desktop.screensaver idle-activation-enabled false
    gsettings set org.gnome.desktop.screensaver lock-enabled false
    gsettings set org.gnome.desktop.screensaver lock-delay 3600

    # Disable the screen saver
    xset s off s reset

    # This starts the gnome-keyring-daemon with an unlocked login keyring. libsecret uses this to
    # store secrets. Firefox uses libsecret to store a key that protects sensitive information like
    # credit card numbers.
    eval `echo '' | /usr/bin/gnome-keyring-daemon -r -d --unlock --components=secrets`

    mount || :
    df -h || :

    # Wait for gnome-shell to start up
    retry_count=10
    while ! dbus-send --print-reply=literal --session --dest=org.gnome.Shell --type=method_call /org/gnome/Shell org.freedesktop.DBus.Properties.Get string:org.gnome.Shell string:OverviewActive; do
      ps auxf || :
      if [ $retry_count = 0 ]; then
        echo "gnome-shell still not up, retrying the task" >&2
        exit 4
      fi
      retry_count=$((retry_count - 1))
      sleep 5
    done
    # Sometimes gnome-shell starts up in overview even though the ubuntu-dock
    # extension is supposed to disable that.  When that happens we never get
    # focus and the test harness times out.  So tell gnome-shell to get out of
    # overview before anything else.
    if dbus-send --print-reply=literal --session --dest=org.gnome.Shell --type=method_call /org/gnome/Shell org.freedesktop.DBus.Properties.Get string:org.gnome.Shell string:OverviewActive | grep true; then
      dbus-send --session --dest=org.gnome.Shell --type=method_call /org/gnome/Shell org.freedesktop.DBus.Properties.Set string:org.gnome.Shell string:OverviewActive variant:boolean:false
      sleep 2
      if dbus-send --print-reply=literal --session --dest=org.gnome.Shell --type=method_call /org/gnome/Shell org.freedesktop.DBus.Properties.Get string:org.gnome.Shell string:OverviewActive | grep true; then
        echo "gnome-shell didn't get out of overview, retrying the task" >&2
        exit 4
      fi
    fi

    # Run mutter as nested wayland compositor to provide Wayland environment
    # on top of XVfb.
    if [ $MOZ_ENABLE_WAYLAND ]; then
      env | grep "DISPLAY"
      mutter --display=:0 --wayland --nested &
      export WAYLAND_DISPLAY=wayland-0
      retry_count=0
      max_retries=5
      until [ $retry_count -gt $max_retries ]; do
        if [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
            retry_count=$(($max_retries + 1))
        else
            retry_count=$(($retry_count + 1))
            echo "Waiting for Mutter, retry: $retry_count"
            sleep 2
        fi
      done
    fi
fi

if [[ $NEED_COMPIZ == true ]]  && [[ $RELEASE == 16.04 ]]; then
    compiz 2>&1 &
elif [[ $NEED_COMPIZ == true ]] && [[ $RELEASE == 18.04 ]]; then
    compiz --replace 2>&1 &
fi

# Bug 1607713 - set cursor position to 0,0 to avoid odd libx11 interaction
if $NEED_WINDOW_MANAGER && [ $DISPLAY == ':0' ]; then
    xwit -root -warp 0 0
fi

maybe_start_pulse

# support multiple, space delimited, config files
config_cmds=""
for cfg in $MOZHARNESS_CONFIG; do
  config_cmds="${config_cmds} --config-file ${MOZHARNESS_PATH}/configs/${cfg}"
done

if [ -n "$MOZHARNESS_OPTIONS" ]; then
    options=""
    for option in $MOZHARNESS_OPTIONS; do
        options="$options --$option"
    done
fi

# Save the computed mozharness command to a binary which is useful for
# interactive mode.
mozharness_bin="$HOME/bin/run-mozharness"
mkdir -p $(dirname $mozharness_bin)

echo -e "#!/usr/bin/env bash
# Some mozharness scripts assume base_work_dir is in
# the current working directory, see bug 1279237
cd "$WORKSPACE"
cmd=\"${PYTHON:-python3} ${MOZHARNESS_PATH}/scripts/${MOZHARNESS_SCRIPT} ${config_cmds} ${options} ${@} \${@}\"
echo \"Running: \${cmd}\"
exec \${cmd}" > ${mozharness_bin}
chmod +x ${mozharness_bin}

# In interactive mode, the user will be prompted with options for what to do.
if ! $TASKCLUSTER_INTERACTIVE; then
  # run the given mozharness script and configs, but pass the rest of the
  # arguments in from our own invocation
  ${mozharness_bin};
fi

# Run a custom mach command (this is typically used by action tasks to run
# harnesses in a particular way)
if [ "$CUSTOM_MACH_COMMAND" ]; then
    eval "'$WORKSPACE/build/venv/bin/python' '$WORKSPACE/build/tests/mach' ${CUSTOM_MACH_COMMAND} ${@}"
    exit $?
fi
