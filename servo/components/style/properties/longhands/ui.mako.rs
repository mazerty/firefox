/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

<%namespace name="helpers" file="/helpers.mako.rs" />
<% from data import DEFAULT_RULES_EXCEPT_KEYFRAME %>

// TODO spec says that UAs should not support this
// we should probably remove from gecko (https://bugzilla.mozilla.org/show_bug.cgi?id=1328331)
${helpers.single_keyword(
    "ime-mode",
    "auto normal active disabled inactive",
    engines="gecko",
    gecko_enum_prefix="StyleImeMode",
    gecko_ffi_name="mIMEMode",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-ui/#input-method-editor",
    affects="",
)}

${helpers.single_keyword(
    "scrollbar-width",
    "auto thin none",
    engines="gecko",
    gecko_enum_prefix="StyleScrollbarWidth",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-scrollbars-1/#scrollbar-width",
    affects="layout",
)}

${helpers.predefined_type(
    "user-select",
    "UserSelect",
    "computed::UserSelect::Auto",
    engines="gecko",
    extra_prefixes="moz webkit",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-ui-4/#propdef-user-select",
    affects="",
)}

${helpers.single_keyword(
    "-moz-window-dragging",
    "default drag no-drag",
    engines="gecko",
    gecko_ffi_name="mWindowDragging",
    gecko_enum_prefix="StyleWindowDragging",
    animation_type="discrete",
    enabled_in="chrome",
    spec="None (Nonstandard Firefox-only property)",
    affects="paint",
)}

// TODO(emilio): Maybe make shadow behavior on macOS match Linux / Windows, and remove this? But
// that requires making -moz-window-input-region-margin work there...
${helpers.single_keyword(
    "-moz-window-shadow",
    "auto none",
    engines="gecko",
    gecko_ffi_name="mWindowShadow",
    gecko_enum_prefix="StyleWindowShadow",
    animation_type="discrete",
    enabled_in="chrome",
    spec="None (Nonstandard internal property)",
    affects="overflow",
)}

${helpers.predefined_type(
    "-moz-window-opacity",
    "Opacity",
    "1.0",
    engines="gecko",
    gecko_ffi_name="mWindowOpacity",
    spec="None (Nonstandard internal property)",
    enabled_in="chrome",
    affects="paint",
)}

${helpers.predefined_type(
    "-moz-window-transform",
    "Transform",
    "generics::transform::Transform::none()",
    engines="gecko",
    spec="None (Nonstandard internal property)",
    enabled_in="chrome",
    affects="overflow",
)}

${helpers.predefined_type(
    "-moz-window-input-region-margin",
    "Length",
    "computed::Length::zero()",
    engines="gecko",
    spec="None (Nonstandard internal property)",
    enabled_in="chrome",
    affects="",
)}

// Hack to allow chrome to hide stuff only visually (without hiding it from a11y).
${helpers.predefined_type(
    "-moz-subtree-hidden-only-visually",
    "BoolInteger",
    "computed::BoolInteger::zero()",
    engines="gecko",
    animation_type="discrete",
    spec="None (Nonstandard internal property)",
    enabled_in="chrome",
    affects="paint",
)}

// TODO(emilio): Probably also should be hidden from content.
${helpers.predefined_type(
    "-moz-force-broken-image-icon",
    "BoolInteger",
    "computed::BoolInteger::zero()",
    engines="gecko",
    animation_type="discrete",
    spec="None (Nonstandard Firefox-only property)",
    affects="layout",
)}

<% transition_extra_prefixes = "moz:layout.css.prefixes.transitions webkit" %>

${helpers.predefined_type(
    "transition-duration",
    "Time",
    "computed::Time::zero()",
    engines="gecko servo",
    initial_specified_value="specified::Time::zero()",
    parse_method="parse_non_negative",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=transition_extra_prefixes,
    spec="https://drafts.csswg.org/css-transitions/#propdef-transition-duration",
    affects="",
)}

${helpers.predefined_type(
    "transition-timing-function",
    "TimingFunction",
    "computed::TimingFunction::ease()",
    engines="gecko servo",
    initial_specified_value="specified::TimingFunction::ease()",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=transition_extra_prefixes,
    spec="https://drafts.csswg.org/css-transitions/#propdef-transition-timing-function",
    affects="",
)}

${helpers.predefined_type(
    "transition-property",
    "TransitionProperty",
    "computed::TransitionProperty::all()",
    engines="gecko servo",
    initial_specified_value="specified::TransitionProperty::all()",
    vector=True,
    none_value="computed::TransitionProperty::none()",
    need_index=True,
    animation_type="none",
    extra_prefixes=transition_extra_prefixes,
    spec="https://drafts.csswg.org/css-transitions/#propdef-transition-property",
    affects="",
)}

${helpers.predefined_type(
    "transition-delay",
    "Time",
    "computed::Time::zero()",
    engines="gecko servo",
    initial_specified_value="specified::Time::zero()",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=transition_extra_prefixes,
    spec="https://drafts.csswg.org/css-transitions/#propdef-transition-delay",
    affects="",
)}

${helpers.predefined_type(
    "transition-behavior",
    "TransitionBehavior",
    "computed::TransitionBehavior::normal()",
    engines="gecko servo",
    initial_specified_value="specified::TransitionBehavior::normal()",
    vector=True,
    need_index=True,
    animation_type="none",
    spec="https://drafts.csswg.org/css-transitions-2/#transition-behavior-property",
    affects="",
)}

<% animation_extra_prefixes = "moz:layout.css.prefixes.animations webkit" %>

${helpers.predefined_type(
    "animation-name",
    "AnimationName",
    "computed::AnimationName::none()",
    engines="gecko servo",
    initial_specified_value="specified::AnimationName::none()",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=animation_extra_prefixes,
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    spec="https://drafts.csswg.org/css-animations/#propdef-animation-name",
    affects="",
)}

${helpers.predefined_type(
    "animation-duration",
    "AnimationDuration",
    "computed::AnimationDuration::auto()",
    engines="gecko servo",
    initial_specified_value="specified::AnimationDuration::auto()",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=animation_extra_prefixes,
    spec="https://drafts.csswg.org/css-animations-2/#animation-duration",
    affects="",
)}

// animation-timing-function is the exception to the rule for allowed_in_keyframe_block:
// https://drafts.csswg.org/css-animations/#keyframes
${helpers.predefined_type(
    "animation-timing-function",
    "TimingFunction",
    "computed::TimingFunction::ease()",
    engines="gecko servo",
    initial_specified_value="specified::TimingFunction::ease()",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=animation_extra_prefixes,
    spec="https://drafts.csswg.org/css-transitions/#propdef-animation-timing-function",
    affects="",
)}

${helpers.predefined_type(
    "animation-iteration-count",
    "AnimationIterationCount",
    "computed::AnimationIterationCount::one()",
    engines="gecko servo",
    initial_specified_value="specified::AnimationIterationCount::one()",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=animation_extra_prefixes,
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    spec="https://drafts.csswg.org/css-animations/#propdef-animation-iteration-count",
    affects="",
)}

${helpers.predefined_type(
    "animation-direction",
    "AnimationDirection",
    "computed::AnimationDirection::Normal",
    engines="gecko servo",
    initial_specified_value="specified::AnimationDirection::Normal",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=animation_extra_prefixes,
    spec="https://drafts.csswg.org/css-animations/#propdef-animation-direction",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.predefined_type(
    "animation-play-state",
    "AnimationPlayState",
    "computed::AnimationPlayState::Running",
    engines="gecko servo",
    initial_specified_value="computed::AnimationPlayState::Running",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=animation_extra_prefixes,
    spec="https://drafts.csswg.org/css-animations/#propdef-animation-play-state",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.predefined_type(
    "animation-fill-mode",
    "AnimationFillMode",
    "computed::AnimationFillMode::None",
    engines="gecko servo",
    initial_specified_value="computed::AnimationFillMode::None",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=animation_extra_prefixes,
    spec="https://drafts.csswg.org/css-animations/#propdef-animation-fill-mode",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.predefined_type(
    "animation-composition",
    "AnimationComposition",
    "computed::AnimationComposition::Replace",
    engines="gecko servo",
    initial_specified_value="computed::AnimationComposition::Replace",
    vector=True,
    need_index=True,
    animation_type="none",
    servo_pref="layout.unimplemented",
    spec="https://drafts.csswg.org/css-animations-2/#animation-composition",
    affects="",
)}

${helpers.predefined_type(
    "animation-delay",
    "Time",
    "computed::Time::zero()",
    engines="gecko servo",
    initial_specified_value="specified::Time::zero()",
    vector=True,
    need_index=True,
    animation_type="none",
    extra_prefixes=animation_extra_prefixes,
    spec="https://drafts.csswg.org/css-animations/#propdef-animation-delay",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.predefined_type(
    "animation-timeline",
    "AnimationTimeline",
    "computed::AnimationTimeline::auto()",
    engines="gecko servo",
    servo_pref="layout.unimplemented",
    initial_specified_value="specified::AnimationTimeline::auto()",
    vector=True,
    need_index=True,
    animation_type="none",
    gecko_pref="layout.css.scroll-driven-animations.enabled",
    spec="https://drafts.csswg.org/css-animations-2/#propdef-animation-timeline",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.predefined_type(
    "scroll-timeline-name",
    "TimelineName",
    "computed::TimelineName::none()",
    vector=True,
    need_index=True,
    engines="gecko",
    animation_type="none",
    gecko_pref="layout.css.scroll-driven-animations.enabled",
    spec="https://drafts.csswg.org/scroll-animations-1/#scroll-timeline-name",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.predefined_type(
    "scroll-timeline-axis",
    "ScrollAxis",
    "computed::ScrollAxis::default()",
    vector=True,
    need_index=True,
    engines="gecko",
    animation_type="none",
    gecko_pref="layout.css.scroll-driven-animations.enabled",
    spec="https://drafts.csswg.org/scroll-animations-1/#scroll-timeline-axis",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.predefined_type(
    "view-timeline-name",
    "TimelineName",
    "computed::TimelineName::none()",
    vector=True,
    need_index=True,
    engines="gecko",
    animation_type="none",
    gecko_pref="layout.css.scroll-driven-animations.enabled",
    spec="https://drafts.csswg.org/scroll-animations-1/#view-timeline-name",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.predefined_type(
    "view-timeline-axis",
    "ScrollAxis",
    "computed::ScrollAxis::default()",
    vector=True,
    need_index=True,
    engines="gecko",
    animation_type="none",
    gecko_pref="layout.css.scroll-driven-animations.enabled",
    spec="https://drafts.csswg.org/scroll-animations-1/#view-timeline-axis",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.predefined_type(
    "view-timeline-inset",
    "ViewTimelineInset",
    "computed::ViewTimelineInset::default()",
    vector=True,
    need_index=True,
    engines="gecko",
    animation_type="none",
    gecko_pref="layout.css.scroll-driven-animations.enabled",
    spec="https://drafts.csswg.org/scroll-animations-1/#view-timeline-axis",
    rule_types_allowed=DEFAULT_RULES_EXCEPT_KEYFRAME,
    affects="",
)}

${helpers.single_keyword(
    "field-sizing",
    "fixed content",
    engines="gecko",
    gecko_enum_prefix="StyleFieldSizing",
    animation_type="discrete",
    gecko_pref="layout.css.field-sizing.enabled",
    spec="https://drafts.csswg.org/css-ui/#field-sizing",
    affects="layout",
)}

${helpers.predefined_type(
    "view-transition-name",
    "ViewTransitionName",
    "computed::ViewTransitionName::none()",
    engines="gecko servo",
    servo_pref="layout.unimplemented",
    animation_type="discrete",
    gecko_pref="dom.viewTransitions.enabled",
    spec="https://drafts.csswg.org/css-view-transitions-1/#view-transition-name-prop",
    affects="",
    enabled_in="ua",
)}

${helpers.predefined_type(
    "view-transition-class",
    "ViewTransitionClass",
    "computed::ViewTransitionClass::none()",
    engines="gecko servo",
    servo_pref="layout.unimplemented",
    animation_type="discrete",
    gecko_pref="dom.viewTransitions.enabled",
    spec="https://drafts.csswg.org/css-view-transitions-2/#view-transition-class-prop",
    affects="",
    enabled_in="ua",
)}
