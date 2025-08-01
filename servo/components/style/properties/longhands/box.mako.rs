/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

<%namespace name="helpers" file="/helpers.mako.rs" />
<% from data import ALL_AXES %>

${helpers.predefined_type(
    "display",
    "Display",
    "computed::Display::inline()",
    engines="gecko servo",
    initial_specified_value="specified::Display::inline()",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-display/#propdef-display",
    servo_restyle_damage="rebuild_and_reflow",
    affects="layout",
)}

${helpers.single_keyword(
    "-moz-top-layer",
    "none auto",
    engines="gecko",
    gecko_enum_prefix="StyleTopLayer",
    gecko_ffi_name="mTopLayer",
    animation_type="discrete",
    enabled_in="ua",
    spec="Internal (not web-exposed)",
    affects="layout",
)}

// An internal-only property for elements in a top layer
// https://fullscreen.spec.whatwg.org/#top-layer
${helpers.single_keyword(
    "-servo-top-layer",
    "none top",
    engines="servo",
    animation_type="none",
    enabled_in="ua",
    spec="Internal (not web-exposed)",
    affects="layout",
)}

${helpers.predefined_type(
    "position",
    "PositionProperty",
    "computed::PositionProperty::Static",
    engines="gecko servo",
    initial_specified_value="specified::PositionProperty::Static",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-position/#position-property",
    servo_restyle_damage="rebuild_and_reflow",
    affects="layout",
)}

// Changes do not invalidate our element. We handle notify/invalidating
// elements that reference anchor-name elsewhere.
${helpers.predefined_type(
    "anchor-name",
    "AnchorName",
    "computed::AnchorName::none()",
    engines="gecko",
    animation_type="discrete",
    gecko_pref="layout.css.anchor-positioning.enabled",
    spec="https://drafts.csswg.org/css-anchor-position-1/#propdef-anchor-name",
    affects="",
)}

// Changes do not invalidate our element. We handle notify/invalidating
// any affected descendants elsewhere.
${helpers.predefined_type(
    "anchor-scope",
    "AnchorScope",
    "computed::AnchorScope::none()",
    engines="gecko",
    animation_type="discrete",
    gecko_pref="layout.css.anchor-positioning.enabled",
    spec="https://drafts.csswg.org/css-anchor-position-1/#propdef-scope",
    affects="",
)}

${helpers.predefined_type(
    "float",
    "Float",
    "computed::Float::None",
    engines="gecko servo",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-box/#propdef-float",
    servo_restyle_damage="rebuild_and_reflow",
    affects="layout",
)}

${helpers.predefined_type(
    "clear",
    "Clear",
    "computed::Clear::None",
    engines="gecko servo",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css2/#propdef-clear",
    servo_restyle_damage="rebuild_and_reflow",
    affects="layout",
)}

${helpers.predefined_type(
    "vertical-align",
    "VerticalAlign",
    "computed::VerticalAlign::baseline()",
    engines="gecko servo",
    spec="https://www.w3.org/TR/CSS2/visudet.html#propdef-vertical-align",
    servo_restyle_damage = "reflow",
    affects="layout",
)}

${helpers.predefined_type(
    "baseline-source",
    "BaselineSource",
    "computed::BaselineSource::Auto",
    engines="gecko servo",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-inline-3/#baseline-source",
    servo_restyle_damage = "reflow",
    affects="layout",
)}

// CSS 2.1, Section 11 - Visual effects

${helpers.single_keyword(
    "-servo-overflow-clip-box",
    "padding-box content-box",
    engines="servo",
    animation_type="none",
    enabled_in="ua",
    spec="Internal, not web-exposed, \
          may be standardized in the future (https://developer.mozilla.org/en-US/docs/Web/CSS/overflow-clip-box)",
    affects="layout",
)}

% for direction in ["inline", "block"]:
    ${helpers.predefined_type(
        "overflow-clip-box-" + direction,
        "OverflowClipBox",
        "computed::OverflowClipBox::PaddingBox",
        engines="gecko",
        enabled_in="chrome",
        gecko_pref="layout.css.overflow-clip-box.enabled",
        animation_type="discrete",
        spec="Internal, may be standardized in the future: \
              https://developer.mozilla.org/en-US/docs/Web/CSS/overflow-clip-box",
        affects="layout",
    )}
% endfor

% for (axis, logical) in ALL_AXES:
    <% full_name = "overflow-{}".format(axis) %>
    ${helpers.predefined_type(
        full_name,
        "Overflow",
        "computed::Overflow::Visible",
        engines="gecko servo",
        logical_group="overflow",
        logical=logical,
        animation_type="discrete",
        spec="https://drafts.csswg.org/css-overflow-3/#propdef-{}".format(full_name),
        servo_restyle_damage = "reflow",
        affects="layout",
    )}
% endfor

${helpers.predefined_type(
    "overflow-anchor",
    "OverflowAnchor",
    "computed::OverflowAnchor::Auto",
    engines="gecko",
    initial_specified_value="specified::OverflowAnchor::Auto",
    gecko_pref="layout.css.scroll-anchoring.enabled",
    spec="https://drafts.csswg.org/css-scroll-anchoring/#exclusion-api",
    animation_type="discrete",
    affects="",
)}

<% transform_extra_prefixes = "moz:layout.css.prefixes.transforms webkit" %>

${helpers.predefined_type(
    "transform",
    "Transform",
    "generics::transform::Transform::none()",
    engines="gecko servo",
    extra_prefixes=transform_extra_prefixes,
    flags="CAN_ANIMATE_ON_COMPOSITOR",
    spec="https://drafts.csswg.org/css-transforms/#propdef-transform",
    servo_restyle_damage="reflow_out_of_flow",
    affects="overflow",
)}

${helpers.predefined_type(
    "rotate",
    "Rotate",
    "generics::transform::Rotate::None",
    engines="gecko servo",
    boxed=True,
    flags="CAN_ANIMATE_ON_COMPOSITOR",
    spec="https://drafts.csswg.org/css-transforms-2/#individual-transforms",
    servo_restyle_damage = "reflow_out_of_flow",
    affects="overflow",
)}

${helpers.predefined_type(
    "scale",
    "Scale",
    "generics::transform::Scale::None",
    engines="gecko servo",
    boxed=True,
    flags="CAN_ANIMATE_ON_COMPOSITOR",
    spec="https://drafts.csswg.org/css-transforms-2/#individual-transforms",
    servo_restyle_damage = "reflow_out_of_flow",
    affects="overflow",
)}

${helpers.predefined_type(
    "translate",
    "Translate",
    "generics::transform::Translate::None",
    engines="gecko servo",
    boxed=True,
    flags="CAN_ANIMATE_ON_COMPOSITOR",
    spec="https://drafts.csswg.org/css-transforms-2/#individual-transforms",
    servo_restyle_damage="reflow_out_of_flow",
    affects="overflow",
)}

// Motion Path Module Level 1
${helpers.predefined_type(
    "offset-path",
    "OffsetPath",
    "computed::OffsetPath::none()",
    engines="gecko servo",
    servo_pref="layout.unimplemented",
    flags="CAN_ANIMATE_ON_COMPOSITOR",
    spec="https://drafts.fxtf.org/motion-1/#offset-path-property",
    servo_restyle_damage="reflow_out_of_flow",
    affects="overflow",
)}

// Motion Path Module Level 1
${helpers.predefined_type(
    "offset-distance",
    "LengthPercentage",
    "computed::LengthPercentage::zero()",
    engines="gecko",
    flags="CAN_ANIMATE_ON_COMPOSITOR",
    spec="https://drafts.fxtf.org/motion-1/#offset-distance-property",
    servo_restyle_damage="reflow_out_of_flow",
    affects="overflow",
)}

// Motion Path Module Level 1
${helpers.predefined_type(
    "offset-rotate",
    "OffsetRotate",
    "computed::OffsetRotate::auto()",
    engines="gecko",
    flags="CAN_ANIMATE_ON_COMPOSITOR",
    spec="https://drafts.fxtf.org/motion-1/#offset-rotate-property",
    servo_restyle_damage="reflow_out_of_flow",
    affects="overflow",
)}

// Motion Path Module Level 1
${helpers.predefined_type(
    "offset-anchor",
    "PositionOrAuto",
    "computed::PositionOrAuto::auto()",
    engines="gecko",
    flags="CAN_ANIMATE_ON_COMPOSITOR",
    spec="https://drafts.fxtf.org/motion-1/#offset-anchor-property",
    servo_restyle_damage="reflow_out_of_flow",
    boxed=True,
    affects="overflow",
)}

// Motion Path Module Level 1
${helpers.predefined_type(
    "offset-position",
    "OffsetPosition",
    "computed::OffsetPosition::normal()",
    engines="gecko",
    flags="CAN_ANIMATE_ON_COMPOSITOR",
    spec="https://drafts.fxtf.org/motion-1/#offset-position-property",
    servo_restyle_damage="reflow_out_of_flow",
    boxed=True,
    affects="overflow",
)}

// CSSOM View Module
// https://www.w3.org/TR/cssom-view-1/
${helpers.single_keyword(
    "scroll-behavior",
    "auto smooth",
    engines="gecko",
    spec="https://drafts.csswg.org/cssom-view/#propdef-scroll-behavior",
    animation_type="discrete",
    gecko_enum_prefix="StyleScrollBehavior",
    affects="",
)}

${helpers.predefined_type(
    "scroll-snap-align",
    "ScrollSnapAlign",
    "computed::ScrollSnapAlign::none()",
    engines="gecko",
    spec="https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-align",
    animation_type="discrete",
    affects="paint",
)}

${helpers.predefined_type(
    "scroll-snap-type",
    "ScrollSnapType",
    "computed::ScrollSnapType::none()",
    engines="gecko",
    spec="https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-type",
    animation_type="discrete",
    affects="paint",
)}

${helpers.predefined_type(
    "scroll-snap-stop",
    "ScrollSnapStop",
    "computed::ScrollSnapStop::Normal",
    engines="gecko",
    spec="https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-stop",
    animation_type="discrete",
    affects="paint",
)}

% for (axis, logical) in ALL_AXES:
    ${helpers.predefined_type(
        "overscroll-behavior-" + axis,
        "OverscrollBehavior",
        "computed::OverscrollBehavior::Auto",
        engines="gecko",
        logical_group="overscroll-behavior",
        logical=logical,
        spec="https://wicg.github.io/overscroll-behavior/#overscroll-behavior-properties",
        animation_type="discrete",
        affects="paint",
    )}
% endfor

// Compositing and Blending Level 1
// http://www.w3.org/TR/compositing-1/
${helpers.single_keyword(
    "isolation",
    "auto isolate",
    engines="gecko servo",
    spec="https://drafts.fxtf.org/compositing/#isolation",
    gecko_enum_prefix="StyleIsolation",
    animation_type="discrete",
    affects="paint",
)}

${helpers.predefined_type(
    "break-after",
    "BreakBetween",
    "computed::BreakBetween::Auto",
    engines="gecko",
    spec="https://drafts.csswg.org/css-break/#propdef-break-after",
    animation_type="discrete",
    affects="layout",
)}

${helpers.predefined_type(
    "break-before",
    "BreakBetween",
    "computed::BreakBetween::Auto",
    engines="gecko",
    spec="https://drafts.csswg.org/css-break/#propdef-break-before",
    animation_type="discrete",
    affects="layout",
)}

${helpers.predefined_type(
    "break-inside",
    "BreakWithin",
    "computed::BreakWithin::Auto",
    engines="gecko",
    spec="https://drafts.csswg.org/css-break/#propdef-break-inside",
    animation_type="discrete",
    affects="layout",
)}

// CSS Basic User Interface Module Level 3
// http://dev.w3.org/csswg/css-ui
${helpers.predefined_type(
    "resize",
    "Resize",
    "computed::Resize::None",
    engines="gecko",
    animation_type="discrete",
    gecko_ffi_name="mResize",
    spec="https://drafts.csswg.org/css-ui/#propdef-resize",
    affects="layout",
)}

${helpers.predefined_type(
    "perspective",
    "Perspective",
    "computed::Perspective::none()",
    engines="gecko servo",
    gecko_ffi_name="mChildPerspective",
    spec="https://drafts.csswg.org/css-transforms/#perspective",
    extra_prefixes=transform_extra_prefixes,
    servo_restyle_damage = "reflow_out_of_flow",
    affects="overflow",
)}

${helpers.predefined_type(
    "perspective-origin",
    "Position",
    "computed::position::Position::center()",
    engines="gecko servo",
    boxed=True,
    extra_prefixes=transform_extra_prefixes,
    spec="https://drafts.csswg.org/css-transforms-2/#perspective-origin-property",
    servo_restyle_damage="reflow_out_of_flow",
    affects="overflow",
)}

${helpers.single_keyword(
    "backface-visibility",
    "visible hidden",
    engines="gecko servo",
    gecko_enum_prefix="StyleBackfaceVisibility",
    spec="https://drafts.csswg.org/css-transforms/#backface-visibility-property",
    extra_prefixes=transform_extra_prefixes,
    animation_type="discrete",
    affects="paint",
)}

${helpers.predefined_type(
    "transform-box",
    "TransformBox",
    "computed::TransformBox::ViewBox",
    engines="gecko",
    spec="https://drafts.csswg.org/css-transforms/#transform-box",
    animation_type="discrete",
    affects="overflow",
)}

${helpers.predefined_type(
    "transform-style",
    "TransformStyle",
    "computed::TransformStyle::Flat",
    engines="gecko servo",
    spec="https://drafts.csswg.org/css-transforms-2/#transform-style-property",
    extra_prefixes=transform_extra_prefixes,
    animation_type="discrete",
    servo_restyle_damage = "reflow_out_of_flow",
    affects="overflow",
)}

${helpers.predefined_type(
    "transform-origin",
    "TransformOrigin",
    "computed::TransformOrigin::initial_value()",
    engines="gecko servo",
    extra_prefixes=transform_extra_prefixes,
    gecko_ffi_name="mTransformOrigin",
    boxed=True,
    spec="https://drafts.csswg.org/css-transforms/#transform-origin-property",
    servo_restyle_damage="reflow_out_of_flow",
    affects="overflow",
)}

${helpers.predefined_type(
    "contain",
    "Contain",
    "specified::Contain::empty()",
    engines="gecko servo",
    servo_pref="layout.unimplemented",
    animation_type="none",
    spec="https://drafts.csswg.org/css-contain/#contain-property",
    affects="layout",
)}

${helpers.predefined_type(
    "content-visibility",
    "ContentVisibility",
    "computed::ContentVisibility::Visible",
    engines="gecko",
    spec="https://drafts.csswg.org/css-contain/#content-visibility",
    affects="layout",
)}

${helpers.predefined_type(
    "container-type",
    "ContainerType",
    "computed::ContainerType::NORMAL",
    engines="gecko servo",
    animation_type="none",
    servo_pref="layout.container-queries.enabled",
    spec="https://drafts.csswg.org/css-contain-3/#container-type",
    affects="layout",
)}

${helpers.predefined_type(
    "container-name",
    "ContainerName",
    "computed::ContainerName::none()",
    engines="gecko servo",
    animation_type="none",
    servo_pref="layout.container-queries.enabled",
    spec="https://drafts.csswg.org/css-contain-3/#container-name",
    affects="",
)}

${helpers.predefined_type(
    "appearance",
    "Appearance",
    "computed::Appearance::None",
    engines="gecko",
    extra_prefixes="moz:layout.css.moz-appearance.enabled webkit",
    spec="https://drafts.csswg.org/css-ui-4/#propdef-appearance",
    animation_type="discrete",
    gecko_ffi_name="mAppearance",
    affects="paint",
)}

// The inherent widget type of an element, selected by specifying
// `appearance: auto`.
${helpers.predefined_type(
    "-moz-default-appearance",
    "Appearance",
    "computed::Appearance::None",
    engines="gecko",
    animation_type="none",
    spec="Internal (not web-exposed)",
    enabled_in="chrome",
    gecko_ffi_name="mDefaultAppearance",
    affects="paint",
)}

${helpers.single_keyword(
    "-moz-orient",
    "inline block horizontal vertical",
    engines="gecko",
    gecko_ffi_name="mOrient",
    gecko_enum_prefix="StyleOrient",
    spec="Nonstandard (https://developer.mozilla.org/en-US/docs/Web/CSS/-moz-orient)",
    animation_type="discrete",
    affects="layout",
)}

${helpers.predefined_type(
    "will-change",
    "WillChange",
    "computed::WillChange::auto()",
    engines="gecko servo",
    animation_type="none",
    spec="https://drafts.csswg.org/css-will-change/#will-change",
    affects="layout",
)}

// The spec issue for the parse_method: https://github.com/w3c/csswg-drafts/issues/4102.
${helpers.predefined_type(
    "shape-image-threshold",
    "Opacity",
    "0.0",
    engines="gecko",
    spec="https://drafts.csswg.org/css-shapes/#shape-image-threshold-property",
    affects="layout",
)}

${helpers.predefined_type(
    "shape-margin",
    "NonNegativeLengthPercentage",
    "computed::NonNegativeLengthPercentage::zero()",
    engines="gecko",
    spec="https://drafts.csswg.org/css-shapes/#shape-margin-property",
    affects="layout",
)}

${helpers.predefined_type(
    "shape-outside",
    "basic_shape::ShapeOutside",
    "generics::basic_shape::ShapeOutside::None",
    engines="gecko",
    spec="https://drafts.csswg.org/css-shapes/#shape-outside-property",
    affects="layout",
)}

${helpers.predefined_type(
    "touch-action",
    "TouchAction",
    "computed::TouchAction::auto()",
    engines="gecko",
    animation_type="discrete",
    spec="https://compat.spec.whatwg.org/#touch-action",
    affects="paint",
)}

${helpers.predefined_type(
    "-webkit-line-clamp",
    "LineClamp",
    "computed::LineClamp::none()",
    engines="gecko",
    spec="https://drafts.csswg.org/css-overflow-3/#line-clamp",
    affects="layout",
)}

${helpers.predefined_type(
    "scrollbar-gutter",
    "ScrollbarGutter",
    "computed::ScrollbarGutter::AUTO",
    engines="gecko",
    animation_type="discrete",
    spec="https://drafts.csswg.org/css-overflow-3/#scrollbar-gutter-property",
    affects="layout",
)}

${helpers.predefined_type(
    "zoom",
    "Zoom",
    "computed::box_::Zoom::ONE",
    engines="gecko servo",
    spec="Non-standard (https://github.com/atanassov/css-zoom/ is the closest)",
    gecko_pref="layout.css.zoom.enabled",
    servo_pref="layout.unimplemented",
    affects="layout",
    enabled_in="chrome",
)}
