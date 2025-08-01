/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

import { actionCreators as ac, actionTypes as at } from "common/Actions.mjs";
import React, { useState, useEffect, useRef, useCallback } from "react";
import { useSelector, batch } from "react-redux";

/**
 * Calculates the remaining time (in seconds) by subtracting elapsed time from the original duration
 *
 * @param duration
 * @param start
 * @returns int
 */
export const calculateTimeRemaining = (duration, start) => {
  const currentTime = Math.floor(Date.now() / 1000);

  // Subtract the elapsed time from initial duration to get time remaining in the timer
  return Math.max(duration - (currentTime - start), 0);
};

/**
 * Converts a number of seconds into a zero-padded MM:SS time string
 *
 * @param seconds
 * @returns string
 */
export const formatTime = seconds => {
  const minutes = Math.floor(seconds / 60)
    .toString()
    .padStart(2, "0");
  const secs = (seconds % 60).toString().padStart(2, "0");
  return `${minutes}:${secs}`;
};

/**
 * Converts a polar coordinate (angle on circle) into a percentage-based [x,y] position for clip-path
 *
 * @param cx
 * @param cy
 * @param radius
 * @param angle
 * @returns string
 */
export const polarToPercent = (cx, cy, radius, angle) => {
  const rad = ((angle - 90) * Math.PI) / 180;
  const x = cx + radius * Math.cos(rad);
  const y = cy + radius * Math.sin(rad);
  return `${x}% ${y}%`;
};

/**
 * Generates a clip-path polygon string that represents a pie slice from 0 degrees
 * to the current progress angle
 *
 * @returns string
 * @param progress
 */
export const getClipPath = progress => {
  const cx = 50;
  const cy = 50;
  const radius = 50;
  // Show some progress right at the start - 6 degrees is just enough to paint a dot once the timer is ticking
  const angle = progress > 0 ? Math.max(progress * 360, 6) : 0;
  const points = [`50% 50%`];

  for (let a = 0; a <= angle; a += 2) {
    points.push(polarToPercent(cx, cy, radius, a));
  }

  return `polygon(${points.join(", ")})`;
};

export const FocusTimer = ({ dispatch }) => {
  const [timeLeft, setTimeLeft] = useState(0);
  // calculated value for the progress circle; 1 = 100%
  const [progress, setProgress] = useState(0);
  const [progressVisible, setProgressVisible] = useState(false);

  const timerType = useSelector(state => state.TimerWidget.timerType);
  const activeMinutesRef = useRef(null);
  const activeSecondsRef = useRef(null);
  const idleMinutesRef = useRef(null);
  const idleSecondsRef = useRef(null);

  const arcRef = useRef(null);

  const timerData = useSelector(state => state.TimerWidget);
  const { duration, initialDuration, startTime, isRunning } =
    timerData[timerType];

  const resetProgressCircle = useCallback(() => {
    if (arcRef?.current) {
      arcRef.current.style.clipPath = "polygon(50% 50%)";
      arcRef.current.style.webkitClipPath = "polygon(50% 50%)";
    }
    setProgress(0);
  }, [arcRef]);

  // If the timer is running, set the progress visibility to true
  // This helps persist progressbar visibility on refresh/opening a new tab
  useEffect(() => {
    if (isRunning) {
      setProgressVisible(true);
    }
  }, [isRunning]);

  useEffect(() => {
    let interval;
    if (isRunning && duration > 0) {
      interval = setInterval(() => {
        const remaining = calculateTimeRemaining(duration, startTime);

        if (remaining <= 0) {
          clearInterval(interval);

          dispatch(
            ac.AlsoToMain({
              type: at.WIDGETS_TIMER_END,
              data: { timerType },
            })
          );

          // animate the progress circle to turn solid green
          setProgress(1);

          // More transitions after a delay to allow the animation above to complete
          setTimeout(() => {
            // progress circle goes back to default grey
            resetProgressCircle();

            // There's more to see!
            setTimeout(() => {
              // progress circle rolls up to show timer in the default state
              setProgressVisible(false);

              // switch over to the other timer type
              dispatch(
                ac.AlsoToMain({
                  type: at.WIDGETS_TIMER_SET_TYPE,
                  data: {
                    timerType: timerType === "focus" ? "break" : "focus",
                  },
                })
              );
            }, 1500);
          }, 1500);
        }

        // using setTimeLeft to trigger a re-render of the component to show live countdown each second
        setTimeLeft(remaining);
        setProgress((initialDuration - remaining) / initialDuration);
      }, 1000);
    }

    // Shows the correct live time in the UI whenever the timer state changes
    const newTime = isRunning
      ? calculateTimeRemaining(duration, startTime)
      : duration;

    setTimeLeft(newTime);

    return () => clearInterval(interval);
  }, [
    isRunning,
    startTime,
    duration,
    initialDuration,
    dispatch,
    resetProgressCircle,
    timerType,
  ]);

  // Update the clip-path of the gradient circle to match the current progress value
  useEffect(() => {
    if (arcRef?.current) {
      arcRef.current.style.clipPath = getClipPath(progress);
    }
  }, [progress]);

  // set timer function
  const setTimerDuration = () => {
    const minutesEl = progressVisible
      ? activeMinutesRef.current
      : idleMinutesRef.current;

    const secondsEl = progressVisible
      ? activeSecondsRef.current
      : idleSecondsRef.current;

    const minutesValue = minutesEl.innerText.trim() || "0";
    const secondsValue = secondsEl.innerText.trim() || "0";

    let minutes = parseInt(minutesValue || "0", 10);
    let seconds = parseInt(secondsValue || "0", 10);

    // Set a limit of 99 minutes
    minutes = Math.min(minutes, 99);
    // Set a limit of 59 seconds
    seconds = Math.min(seconds, 59);

    const totalSeconds = minutes * 60 + seconds;

    if (
      !Number.isNaN(totalSeconds) &&
      totalSeconds > 0 &&
      totalSeconds !== duration
    ) {
      dispatch(
        ac.AlsoToMain({
          type: at.WIDGETS_TIMER_SET_DURATION,
          data: { timerType, duration: totalSeconds },
        })
      );
    }
  };

  // Pause timer function
  const toggleTimer = () => {
    if (!isRunning && duration > 0) {
      setProgressVisible(true);

      dispatch(
        ac.AlsoToMain({
          type: at.WIDGETS_TIMER_PLAY,
          data: { timerType },
        })
      );
    } else if (isRunning) {
      // calculated to get the new baseline of the timer when it starts or resumes
      const remaining = calculateTimeRemaining(duration, startTime);

      dispatch(
        ac.AlsoToMain({
          type: at.WIDGETS_TIMER_PAUSE,
          data: {
            timerType,
            duration: remaining,
          },
        })
      );
    }
  };

  // reset timer function
  const resetTimer = () => {
    dispatch(
      ac.AlsoToMain({
        type: at.WIDGETS_TIMER_RESET,
        data: { timerType },
      })
    );

    // Reset progress value and gradient arc on the progress circle
    resetProgressCircle();

    // Transition the timer back to the default state if it was expanded
    if (progressVisible) {
      setProgressVisible(false);
    }
  };

  // Toggles between "focus" and "break" timer types
  const toggleType = type => {
    const oldTypeRemaining = calculateTimeRemaining(duration, startTime);

    batch(() => {
      // The type we are toggling away from automatically pauses
      dispatch(
        ac.AlsoToMain({
          type: at.WIDGETS_TIMER_PAUSE,
          data: {
            timerType,
            duration: oldTypeRemaining,
          },
        })
      );

      // Sets the current timer type so it persists when opening a new tab
      dispatch(
        ac.AlsoToMain({
          type: at.WIDGETS_TIMER_SET_TYPE,
          data: {
            timerType: type,
          },
        })
      );
    });
  };

  const handleKeyDown = e => {
    if (e.key === "Enter") {
      e.preventDefault();
      setTimerDuration(e);
    }

    if (e.key === "Tab") {
      setTimerDuration(e);
    }
  };

  const handleBeforeInput = e => {
    const input = e.data;
    const values = e.target.innerText.trim();

    // only allow numerical digits 0–9 for time input
    if (!/^\d+$/.test(input)) {
      e.preventDefault();
    }

    // only allow 2 values each for minutes and seconds
    if (values.length >= 2) {
      e.preventDefault();
    }

    const selection = window.getSelection();
    const selectedText = selection.toString();

    // if entire value is selected, replace it with the new input
    if (selectedText === values) {
      e.preventDefault(); // prevent default typing
      e.target.innerText = input;

      // Places the caret at the end of the content-editable text
      // This is a known problem with content-editable where the caret
      const range = document.createRange();
      range.selectNodeContents(e.target);
      range.collapse(false);
      const sel = window.getSelection();
      sel.removeAllRanges();
      sel.addRange(range);
    }
  };

  const handleFocus = e => {
    if (isRunning) {
      // calculated to get the new baseline of the timer when it starts or resumes
      const remaining = calculateTimeRemaining(duration, startTime);

      dispatch(
        ac.AlsoToMain({
          type: at.WIDGETS_TIMER_PAUSE,
          data: {
            timerType,
            duration: remaining,
          },
        })
      );
    }

    // highlight entire text when focused on the time.
    // this makes it easier to input the new time instead of backspacing
    const el = e.target;
    if (document.createRange && window.getSelection) {
      const range = document.createRange();
      range.selectNodeContents(el);
      const sel = window.getSelection();
      sel.removeAllRanges();
      sel.addRange(range);
    }
  };

  return timerData ? (
    <article className="focus-timer">
      <div className="focus-timer-tabs">
        <moz-button
          type={timerType === "focus" ? "primary" : "ghost"}
          label="Focus"
          onClick={() => toggleType("focus")}
        />
        <moz-button
          type={timerType === "break" ? "primary" : "ghost"}
          label="Break"
          onClick={() => toggleType("break")}
        />
      </div>

      <div
        role="progress"
        className={`progress-circle-wrapper${progressVisible ? " visible" : ""}`}
      >
        <div className="progress-circle-background" />

        <div
          className={`progress-circle ${timerType === "focus" ? "focus-visible" : "focus-hidden"}`}
          ref={timerType === "focus" ? arcRef : null}
        />

        <div
          className={`progress-circle ${timerType === "break" ? "progress-circle-break break-visible" : "break-hidden"}`}
          ref={timerType === "break" ? arcRef : null}
        />

        <div
          className={`progress-circle-complete${progress === 1 ? " visible" : ""}`}
        />
        {progressVisible && (
          <div role="timer" className="progress-circle-label">
            <EditableTimerFields
              minutesRef={activeMinutesRef}
              secondsRef={activeSecondsRef}
              onKeyDown={handleKeyDown}
              onBeforeInput={handleBeforeInput}
              onFocus={handleFocus}
              timeLeft={timeLeft}
              onBlur={() => setTimerDuration()}
            />
          </div>
        )}
      </div>

      <div className="set-timer-controls-wrapper">
        <div
          role="timer"
          className={`set-timer-countdown progress-circle-label${progressVisible ? " hidden" : ""}`}
          aria-hidden={progressVisible}
        >
          <EditableTimerFields
            minutesRef={idleMinutesRef}
            secondsRef={idleSecondsRef}
            onKeyDown={handleKeyDown}
            onBeforeInput={handleBeforeInput}
            onFocus={handleFocus}
            timeLeft={timeLeft}
            tabIndex={progressVisible ? -1 : 0}
            onBlur={() => setTimerDuration()}
          />
        </div>

        <div
          className={`focus-timer-controls ${progressVisible ? "timer-running" : " "}`}
        >
          <moz-button
            type="primary"
            iconsrc={`chrome://global/skin/media/${isRunning ? "pause" : "play"}-fill.svg`}
            title={isRunning ? "Pause" : "Play"}
            onClick={toggleTimer}
          />
          <moz-button
            type="icon ghost"
            iconsrc="chrome://newtab/content/data/content/assets/arrow-clockwise-16.svg"
            title="Reset"
            onClick={resetTimer}
          />
        </div>
      </div>
    </article>
  ) : null;
};

function EditableTimerFields({
  minutesRef,
  secondsRef,
  tabIndex = 0,
  ...props
}) {
  return (
    <>
      <span
        contentEditable="true"
        ref={minutesRef}
        className="timer-set-minutes"
        onKeyDown={props.onKeyDown}
        onBeforeInput={props.onBeforeInput}
        onFocus={props.onFocus}
        onBlur={props.onBlur}
        tabIndex={tabIndex}
      >
        {formatTime(props.timeLeft).split(":")[0]}
      </span>
      :
      <span
        contentEditable="true"
        ref={secondsRef}
        className="timer-set-seconds"
        onKeyDown={props.onKeyDown}
        onBeforeInput={props.onBeforeInput}
        onFocus={props.onFocus}
        onBlur={props.onBlur}
        tabIndex={tabIndex}
      >
        {formatTime(props.timeLeft).split(":")[1]}
      </span>
    </>
  );
}
