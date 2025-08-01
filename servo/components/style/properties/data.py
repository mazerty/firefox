# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

import re
from counted_unknown_properties import COUNTED_UNKNOWN_PROPERTIES

# It is important that the order of these physical / logical variants matches
# the order of the enum variants in logical_geometry.rs
PHYSICAL_SIDES = ["top", "right", "bottom", "left"]
PHYSICAL_CORNERS = ["top-left", "top-right", "bottom-right", "bottom-left"]
PHYSICAL_AXES = ["y", "x"]
PHYSICAL_SIZES = ["height", "width"]
LOGICAL_SIDES = ["block-start", "block-end", "inline-start", "inline-end"]
LOGICAL_CORNERS = ["start-start", "start-end", "end-start", "end-end"]
LOGICAL_SIZES = ["block-size", "inline-size"]
LOGICAL_AXES = ["block", "inline"]

# bool is True when logical
ALL_SIDES = [(side, False) for side in PHYSICAL_SIDES] + [
    (side, True) for side in LOGICAL_SIDES
]
ALL_SIZES = [(size, False) for size in PHYSICAL_SIZES] + [
    (size, True) for size in LOGICAL_SIZES
]
ALL_CORNERS = [(corner, False) for corner in PHYSICAL_CORNERS] + [
    (corner, True) for corner in LOGICAL_CORNERS
]
ALL_AXES = [(axis, False) for axis in PHYSICAL_AXES] + [
    (axis, True) for axis in LOGICAL_AXES
]

SYSTEM_FONT_LONGHANDS = """font_family font_size font_style
                           font_stretch font_weight""".split()

PRIORITARY_PROPERTIES = set(
    [
        # The writing-mode group has the most priority of all property groups, as
        # sizes like font-size can depend on it.
        "writing-mode",
        "direction",
        "text-orientation",
        # The fonts and colors group has the second priority, as all other lengths
        # and colors depend on them.
        #
        # There are some interdependencies between these, but we fix them up in
        # Cascade::fixup_font_stuff.
        # Needed to properly compute the zoomed font-size.
        "-x-text-scale",
        # Needed to do font-size computation in a language-dependent way.
        "-x-lang",
        # Needed for ruby to respect language-dependent min-font-size
        # preferences properly, see bug 1165538.
        "-moz-min-font-size-ratio",
        # font-size depends on math-depth's computed value.
        "math-depth",
        # Needed to compute the first available font and its used size,
        # in order to compute font-relative units correctly.
        "font-size",
        "font-size-adjust",
        "font-weight",
        "font-stretch",
        "font-style",
        "font-family",
        # color-scheme affects how system colors and light-dark() resolve.
        "color-scheme",
        # forced-color-adjust affects whether colors are adjusted.
        "forced-color-adjust",
        # Zoom affects all absolute lengths.
        "zoom",
        # Line height lengths depend on this.
        "line-height",
    ]
)

VISITED_DEPENDENT_PROPERTIES = set(
    [
        "column-rule-color",
        "text-emphasis-color",
        "-webkit-text-fill-color",
        "-webkit-text-stroke-color",
        "text-decoration-color",
        "fill",
        "stroke",
        "caret-color",
        "background-color",
        "border-top-color",
        "border-right-color",
        "border-bottom-color",
        "border-left-color",
        "border-block-start-color",
        "border-inline-end-color",
        "border-block-end-color",
        "border-inline-start-color",
        "outline-color",
        "color",
    ]
)

# Bitfield values for all rule types which can have property declarations.
STYLE_RULE = 1 << 0
PAGE_RULE = 1 << 1
KEYFRAME_RULE = 1 << 2
POSITION_TRY_RULE = 1 << 3
SCOPE_RULE = 1 << 4

ALL_RULES = STYLE_RULE | PAGE_RULE | KEYFRAME_RULE | SCOPE_RULE
DEFAULT_RULES = STYLE_RULE | KEYFRAME_RULE | SCOPE_RULE
DEFAULT_RULES_AND_PAGE = DEFAULT_RULES | PAGE_RULE
DEFAULT_RULES_EXCEPT_KEYFRAME = STYLE_RULE | SCOPE_RULE
DEFAULT_RULES_AND_POSITION_TRY = DEFAULT_RULES | POSITION_TRY_RULE

# Rule name to value dict
RULE_VALUES = {
    "Style": STYLE_RULE,
    "Page": PAGE_RULE,
    "Keyframe": KEYFRAME_RULE,
    "PositionTry": POSITION_TRY_RULE,
    "Scope": SCOPE_RULE,
}


def rule_values_from_arg(that):
    if isinstance(that, int):
        return that
    mask = 0
    for rule in that.split():
        mask |= RULE_VALUES[rule]
    return mask


def maybe_moz_logical_alias(engine, side, prop):
    if engine == "gecko" and side[1]:
        axis, dir = side[0].split("-")
        if axis == "inline":
            return prop % dir
    return None


def to_rust_ident(name):
    name = name.replace("-", "_")
    if name in ["static", "super", "box", "move"]:  # Rust keywords
        name += "_"
    return name


def to_snake_case(ident):
    return re.sub("([A-Z]+)", lambda m: "_" + m.group(1).lower(), ident).strip("_")


def to_camel_case(ident):
    return re.sub(
        "(^|_|-)([a-z0-9])", lambda m: m.group(2).upper(), ident.strip("_").strip("-")
    )


def to_camel_case_lower(ident):
    camel = to_camel_case(ident)
    return camel[0].lower() + camel[1:]


# https://drafts.csswg.org/cssom/#css-property-to-idl-attribute
def to_idl_name(ident):
    return re.sub("-([a-z])", lambda m: m.group(1).upper(), ident)


def parse_aliases(value):
    aliases = {}
    for pair in value.split():
        [a, v] = pair.split("=")
        aliases[a] = v
    return aliases


class Keyword(object):
    def __init__(
        self,
        name,
        values,
        gecko_constant_prefix=None,
        gecko_enum_prefix=None,
        custom_consts=None,
        extra_gecko_values=None,
        extra_servo_values=None,
        gecko_aliases=None,
        servo_aliases=None,
        gecko_strip_moz_prefix=None,
        gecko_inexhaustive=None,
    ):
        self.name = name
        self.values = values.split()
        if gecko_constant_prefix and gecko_enum_prefix:
            raise TypeError(
                "Only one of gecko_constant_prefix and gecko_enum_prefix "
                "can be specified"
            )
        self.gecko_constant_prefix = (
            gecko_constant_prefix or "NS_STYLE_" + self.name.upper().replace("-", "_")
        )
        self.gecko_enum_prefix = gecko_enum_prefix
        self.extra_gecko_values = (extra_gecko_values or "").split()
        self.extra_servo_values = (extra_servo_values or "").split()
        self.gecko_aliases = parse_aliases(gecko_aliases or "")
        self.servo_aliases = parse_aliases(servo_aliases or "")
        self.consts_map = {} if custom_consts is None else custom_consts
        self.gecko_strip_moz_prefix = (
            True if gecko_strip_moz_prefix is None else gecko_strip_moz_prefix
        )
        self.gecko_inexhaustive = gecko_inexhaustive or (gecko_enum_prefix is None)

    def values_for(self, engine):
        if engine == "gecko":
            return self.values + self.extra_gecko_values
        elif engine == "servo":
            return self.values + self.extra_servo_values
        else:
            raise Exception("Bad engine: " + engine)

    def aliases_for(self, engine):
        if engine == "gecko":
            return self.gecko_aliases
        elif engine == "servo":
            return self.servo_aliases
        else:
            raise Exception("Bad engine: " + engine)

    def gecko_constant(self, value):
        moz_stripped = (
            value.replace("-moz-", "")
            if self.gecko_strip_moz_prefix
            else value.replace("-moz-", "moz-")
        )
        mapped = self.consts_map.get(value)
        if self.gecko_enum_prefix:
            parts = moz_stripped.replace("-", "_").split("_")
            parts = mapped if mapped else [p.title() for p in parts]
            return self.gecko_enum_prefix + "::" + "".join(parts)
        else:
            suffix = mapped if mapped else moz_stripped.replace("-", "_")
            return self.gecko_constant_prefix + "_" + suffix.upper()

    def needs_cast(self):
        return self.gecko_enum_prefix is None

    def maybe_cast(self, type_str):
        return "as " + type_str if self.needs_cast() else ""

    def casted_constant_name(self, value, cast_type):
        if cast_type is None:
            raise TypeError("We should specify the cast_type.")

        if self.gecko_enum_prefix is None:
            return cast_type.upper() + "_" + self.gecko_constant(value)
        else:
            return (
                cast_type.upper()
                + "_"
                + self.gecko_constant(value).upper().replace("::", "_")
            )


def arg_to_bool(arg):
    if isinstance(arg, bool):
        return arg
    assert arg in ["True", "False"], "Unexpected value for boolean arguement: " + repr(
        arg
    )
    return arg == "True"


def parse_property_aliases(alias_list):
    result = []
    if alias_list:
        for alias in alias_list.split():
            (name, _, pref) = alias.partition(":")
            result.append((name, pref))
    return result


def to_phys(name, logical, physical):
    return name.replace(logical, physical).replace("inset-", "")


class Property(object):
    def __init__(
        self,
        name,
        spec,
        servo_pref,
        gecko_pref,
        enabled_in,
        rule_types_allowed,
        aliases,
        extra_prefixes,
        flags,
    ):
        self.name = name
        if not spec:
            raise TypeError("Spec should be specified for " + name)
        self.spec = spec
        self.ident = to_rust_ident(name)
        self.camel_case = to_camel_case(self.ident)
        self.servo_pref = servo_pref
        self.gecko_pref = gecko_pref
        self.rule_types_allowed = rule_values_from_arg(rule_types_allowed)
        # For enabled_in, the setup is as follows:
        # It needs to be one of the four values: ["", "ua", "chrome", "content"]
        #  * "chrome" implies "ua", and implies that they're explicitly
        #    enabled.
        #  * "" implies the property will never be parsed.
        #  * "content" implies the property is accessible unconditionally,
        #    modulo a pref, set via servo_pref / gecko_pref.
        assert enabled_in in ("", "ua", "chrome", "content")
        self.enabled_in = enabled_in
        self.aliases = parse_property_aliases(aliases)
        self.extra_prefixes = parse_property_aliases(extra_prefixes)
        self.flags = flags.split() if flags else []

    def rule_types_allowed_names(self):
        for name in RULE_VALUES:
            if self.rule_types_allowed & RULE_VALUES[name] != 0:
                yield name

    def experimental(self, engine):
        if engine == "gecko":
            return bool(self.gecko_pref)
        elif engine == "servo":
            return bool(self.servo_pref)
        else:
            raise Exception("Bad engine: " + engine)

    def explicitly_enabled_in_ua_sheets(self):
        return self.enabled_in in ("ua", "chrome")

    def explicitly_enabled_in_chrome(self):
        return self.enabled_in == "chrome"

    def enabled_in_content(self):
        return self.enabled_in == "content"

    def is_visited_dependent(self):
        return self.name in VISITED_DEPENDENT_PROPERTIES

    def is_prioritary(self):
        return self.name in PRIORITARY_PROPERTIES

    def nscsspropertyid(self):
        return "nsCSSPropertyID::eCSSProperty_" + self.ident


class Longhand(Property):
    def __init__(
        self,
        style_struct,
        name,
        spec=None,
        animation_type=None,
        keyword=None,
        predefined_type=None,
        servo_pref=None,
        gecko_pref=None,
        enabled_in="content",
        need_index=False,
        gecko_ffi_name=None,
        has_effect_on_gecko_scrollbars=None,
        rule_types_allowed=DEFAULT_RULES,
        cast_type="u8",
        logical=False,
        logical_group=None,
        aliases=None,
        extra_prefixes=None,
        boxed=False,
        flags=None,
        allow_quirks="No",
        ignored_when_colors_disabled=False,
        simple_vector_bindings=False,
        vector=False,
        servo_restyle_damage="repaint",
        affects=None,
    ):
        Property.__init__(
            self,
            name=name,
            spec=spec,
            servo_pref=servo_pref,
            gecko_pref=gecko_pref,
            enabled_in=enabled_in,
            rule_types_allowed=rule_types_allowed,
            aliases=aliases,
            extra_prefixes=extra_prefixes,
            flags=flags,
        )

        self.affects = affects
        self.flags += self.affects_flags()

        self.keyword = keyword
        self.predefined_type = predefined_type
        self.style_struct = style_struct
        self.has_effect_on_gecko_scrollbars = has_effect_on_gecko_scrollbars
        assert (
            has_effect_on_gecko_scrollbars in [None, False, True]
            and not style_struct.inherited
            or (gecko_pref is None and enabled_in != "")
            == (has_effect_on_gecko_scrollbars is None)
        ), (
            "Property "
            + name
            + ": has_effect_on_gecko_scrollbars must be "
            + "specified, and must have a value of True or False, iff a "
            + "property is inherited and is behind a Gecko pref or internal"
        )
        self.need_index = need_index
        self.gecko_ffi_name = gecko_ffi_name or "m" + self.camel_case
        self.cast_type = cast_type
        self.logical = arg_to_bool(logical)
        self.logical_group = logical_group
        if self.logical:
            assert logical_group, "Property " + name + " must have a logical group"

        self.boxed = arg_to_bool(boxed)
        self.allow_quirks = allow_quirks
        self.ignored_when_colors_disabled = ignored_when_colors_disabled
        self.is_vector = vector
        self.simple_vector_bindings = simple_vector_bindings

        # This is done like this since just a plain bool argument seemed like
        # really random.
        if animation_type is None:
            animation_type = "normal"
        assert animation_type in ["none", "normal", "discrete"]
        self.animation_type = animation_type
        self.animatable = animation_type != "none"

        # See compute_damage for the various values this can take
        self.servo_restyle_damage = servo_restyle_damage

    def affects_flags(self):
        # Layout is the stronger hint. This property animation affects layout
        # or frame construction. `display` or `width` are examples that should
        # use this.
        if self.affects == "layout":
            return ["AFFECTS_LAYOUT"]
        # This property doesn't affect layout, but affects overflow.
        # `transform` and co. are examples of this.
        if self.affects == "overflow":
            return ["AFFECTS_OVERFLOW"]
        # This property affects the rendered output but doesn't affect layout.
        # `opacity`, `color`, or `z-index` are examples of this.
        if self.affects == "paint":
            return ["AFFECTS_PAINT"]
        # This property doesn't affect rendering in any way.
        # `user-select` is an example of this.
        assert self.affects == "", (
            "Property "
            + self.name
            + ': affects must be specified and be one of ["layout", "overflow", "paint", ""], see Longhand.affects_flags for documentation'
        )
        return []

    @staticmethod
    def type():
        return "longhand"

    # For a given logical property, return the kind of mapping we need to
    # perform, and which logical value we represent, in a tuple.
    def logical_mapping_data(self, data):
        if not self.logical:
            return []
        # Sizes and axes are basically the same for mapping, we just need
        # slightly different replacements (block-size -> height, etc rather
        # than -x/-y) below.
        for [ty, logical_items, physical_items] in [
            ["Side", LOGICAL_SIDES, PHYSICAL_SIDES],
            ["Corner", LOGICAL_CORNERS, PHYSICAL_CORNERS],
            ["Axis", LOGICAL_SIZES, PHYSICAL_SIZES],
            ["Axis", LOGICAL_AXES, PHYSICAL_AXES],
        ]:
            candidate = [s for s in logical_items if s in self.name]
            if candidate:
                assert len(candidate) == 1
                return [ty, candidate[0], logical_items, physical_items]
        assert False, "Don't know how to deal with " + self.name

    def logical_mapping_kind(self, data):
        assert self.logical
        [kind, item, _, _] = self.logical_mapping_data(data)
        return "LogicalMappingKind::{}(Logical{}::{})".format(
            kind, kind, to_camel_case(item.replace("-size", ""))
        )

    # For a given logical property return all the physical property names
    # corresponding to it.
    def all_physical_mapped_properties(self, data):
        if not self.logical:
            return []
        [_, logical_side, _, physical_items] = self.logical_mapping_data(data)
        return [
            data.longhands_by_name[to_phys(self.name, logical_side, physical_side)]
            for physical_side in physical_items
        ]

    def may_be_disabled_in(self, shorthand, engine):
        if engine == "gecko":
            return self.gecko_pref and self.gecko_pref != shorthand.gecko_pref
        elif engine == "servo":
            return self.servo_pref and self.servo_pref != shorthand.servo_pref
        else:
            raise Exception("Bad engine: " + engine)

    def base_type(self):
        if self.predefined_type and not self.is_vector:
            return "crate::values::specified::{}".format(self.predefined_type)
        return "longhands::{}::SpecifiedValue".format(self.ident)

    def specified_type(self):
        if self.predefined_type and not self.is_vector:
            ty = "crate::values::specified::{}".format(self.predefined_type)
        else:
            ty = "longhands::{}::SpecifiedValue".format(self.ident)
        if self.boxed:
            ty = "Box<{}>".format(ty)
        return ty

    def is_zoom_dependent(self):
        if not self.predefined_type:
            return False
        # TODO: Get this from SpecifiedValueInfo or so instead; see bug 1887627.
        return self.predefined_type in {
            "BorderSpacing",
            "FontSize",
            "Inset",
            "Length",
            "LengthPercentage",
            "LengthPercentageOrAuto",
            "LetterSpacing",
            "LineHeight",
            "LineWidth",
            "MaxSize",
            "NonNegativeLength",
            "NonNegativeLengthOrAuto",
            "NonNegativeLengthOrNumber",
            "NonNegativeLengthOrNumberRect",
            "NonNegativeLengthPercentage",
            "NonNegativeLengthPercentageOrNormal",
            "Position",
            "PositionOrAuto",
            "SimpleShadow",
            "Size",
            "SVGLength",
            "SVGStrokeDashArray",
            "SVGWidth",
            "TextDecorationLength",
            "TextIndent",
            "WordSpacing",
        }

    def is_inherited_zoom_dependent_property(self):
        if self.logical:
            return False
        if not self.style_struct.inherited:
            return False
        return self.is_zoom_dependent()

    def specified_is_copy(self):
        if self.is_vector or self.boxed:
            return False
        if self.predefined_type:
            return self.predefined_type in {
                "AlignContent",
                "AlignItems",
                "AlignSelf",
                "Appearance",
                "AnimationComposition",
                "AnimationDirection",
                "AnimationFillMode",
                "AnimationPlayState",
                "AspectRatio",
                "BaselineSource",
                "BreakBetween",
                "BreakWithin",
                "BackgroundRepeat",
                "BorderImageRepeat",
                "BorderStyle",
                "table::CaptionSide",
                "Clear",
                "ColumnCount",
                "Contain",
                "ContentVisibility",
                "ContainerType",
                "Display",
                "FillRule",
                "Float",
                "FontLanguageOverride",
                "FontSizeAdjust",
                "FontStretch",
                "FontStyle",
                "FontSynthesis",
                "FontSynthesisStyle",
                "FontVariantEastAsian",
                "FontVariantLigatures",
                "FontVariantNumeric",
                "FontWeight",
                "GreaterThanOrEqualToOneNumber",
                "GridAutoFlow",
                "ImageRendering",
                "Inert",
                "InitialLetter",
                "Integer",
                "PositionArea",
                "PositionAreaKeyword",
                "PositionProperty",
                "JustifyContent",
                "JustifyItems",
                "JustifySelf",
                "LineBreak",
                "LineClamp",
                "MasonryAutoFlow",
                "MozTheme",
                "BoolInteger",
                "text::MozControlCharacterVisibility",
                "MathDepth",
                "MozScriptMinSize",
                "MozScriptSizeMultiplier",
                "TransformBox",
                "TextDecorationSkipInk",
                "NonNegativeNumber",
                "OffsetRotate",
                "Opacity",
                "OutlineStyle",
                "Overflow",
                "OverflowAnchor",
                "OverflowClipBox",
                "OverflowWrap",
                "OverscrollBehavior",
                "PageOrientation",
                "Percentage",
                "PointerEvents",
                "PositionTryOrder",
                "PositionVisibility",
                "PrintColorAdjust",
                "ForcedColorAdjust",
                "Resize",
                "RubyPosition",
                "SVGOpacity",
                "SVGPaintOrder",
                "ScrollbarGutter",
                "ScrollSnapAlign",
                "ScrollSnapAxis",
                "ScrollSnapStop",
                "ScrollSnapStrictness",
                "ScrollSnapType",
                "TextAlign",
                "TextAlignLast",
                "TextDecorationLine",
                "TextEmphasisPosition",
                "TextJustify",
                "TextTransform",
                "TextUnderlinePosition",
                "TouchAction",
                "TransformStyle",
                "UserFocus",
                "UserInput",
                "UserSelect",
                "VectorEffect",
                "WordBreak",
                "WritingModeProperty",
                "XSpan",
                "XTextScale",
                "ZIndex",
                "Zoom",
            }
        if self.name == "overflow-y":
            return True
        return bool(self.keyword)

    def animated_type(self):
        assert self.animatable
        computed = "<{} as ToComputedValue>::ComputedValue".format(self.base_type())
        if self.animation_type == "discrete":
            return computed
        return "<{} as ToAnimatedValue>::AnimatedValue".format(computed)


class Shorthand(Property):
    def __init__(
        self,
        name,
        sub_properties,
        spec=None,
        servo_pref=None,
        gecko_pref=None,
        enabled_in="content",
        rule_types_allowed=DEFAULT_RULES,
        aliases=None,
        extra_prefixes=None,
        flags=None,
    ):
        Property.__init__(
            self,
            name=name,
            spec=spec,
            servo_pref=servo_pref,
            gecko_pref=gecko_pref,
            enabled_in=enabled_in,
            rule_types_allowed=rule_types_allowed,
            aliases=aliases,
            extra_prefixes=extra_prefixes,
            flags=flags,
        )
        self.sub_properties = sub_properties

    def get_animatable(self):
        for sub in self.sub_properties:
            if sub.animatable:
                return True
        return False

    animatable = property(get_animatable)

    @staticmethod
    def type():
        return "shorthand"


class Alias(object):
    def __init__(self, name, original, gecko_pref):
        self.name = name
        self.ident = to_rust_ident(name)
        self.camel_case = to_camel_case(self.ident)
        self.original = original
        self.enabled_in = original.enabled_in
        self.animatable = original.animatable
        self.servo_pref = original.servo_pref
        self.gecko_pref = gecko_pref
        self.rule_types_allowed = original.rule_types_allowed
        self.flags = original.flags

    @staticmethod
    def type():
        return "alias"

    def rule_types_allowed_names(self):
        for name in RULE_VALUES:
            if self.rule_types_allowed & RULE_VALUES[name] != 0:
                yield name

    def experimental(self, engine):
        if engine == "gecko":
            return bool(self.gecko_pref)
        elif engine == "servo":
            return bool(self.servo_pref)
        else:
            raise Exception("Bad engine: " + engine)

    def explicitly_enabled_in_ua_sheets(self):
        return self.enabled_in in ["ua", "chrome"]

    def explicitly_enabled_in_chrome(self):
        return self.enabled_in == "chrome"

    def enabled_in_content(self):
        return self.enabled_in == "content"

    def nscsspropertyid(self):
        return "nsCSSPropertyID::eCSSPropertyAlias_%s" % self.ident


class Method(object):
    def __init__(self, name, return_type=None, arg_types=None, is_mut=False):
        self.name = name
        self.return_type = return_type
        self.arg_types = arg_types or []
        self.is_mut = is_mut

    def arg_list(self):
        args = ["_: " + x for x in self.arg_types]
        args = ["&mut self" if self.is_mut else "&self"] + args
        return ", ".join(args)

    def signature(self):
        sig = "fn %s(%s)" % (self.name, self.arg_list())
        if self.return_type:
            sig = sig + " -> " + self.return_type
        return sig

    def declare(self):
        return self.signature() + ";"

    def stub(self):
        return self.signature() + "{ unimplemented!() }"


class StyleStruct(object):
    def __init__(self, name, inherited, gecko_name=None):
        self.gecko_struct_name = "Gecko" + name
        self.name = name
        self.name_lower = to_snake_case(name)
        self.ident = to_rust_ident(self.name_lower)
        self.longhands = []
        self.inherited = inherited
        self.gecko_name = gecko_name or name
        self.gecko_ffi_name = "nsStyle" + self.gecko_name
        self.document_dependent = self.gecko_name in ["Font", "Visibility", "Text"]


class PropertiesData(object):
    def __init__(self, engine):
        self.engine = engine
        self.longhands = []
        self.longhands_by_name = {}
        self.longhands_by_logical_group = {}
        self.longhand_aliases = []
        self.shorthands = []
        self.shorthands_by_name = {}
        self.shorthand_aliases = []
        self.counted_unknown_properties = [
            CountedUnknownProperty(p) for p in COUNTED_UNKNOWN_PROPERTIES
        ]

        self.style_structs = [
            StyleStruct("Background", inherited=False),
            StyleStruct("Border", inherited=False),
            StyleStruct("Box", inherited=False, gecko_name="Display"),
            StyleStruct("Column", inherited=False),
            StyleStruct("Counters", inherited=False, gecko_name="Content"),
            StyleStruct("Effects", inherited=False),
            StyleStruct("Font", inherited=True),
            StyleStruct("InheritedBox", inherited=True, gecko_name="Visibility"),
            StyleStruct("InheritedSVG", inherited=True, gecko_name="SVG"),
            StyleStruct("InheritedTable", inherited=True, gecko_name="TableBorder"),
            StyleStruct("InheritedText", inherited=True, gecko_name="Text"),
            StyleStruct("InheritedUI", inherited=True, gecko_name="UI"),
            StyleStruct("List", inherited=True),
            StyleStruct("Margin", inherited=False),
            StyleStruct("Outline", inherited=False),
            StyleStruct("Padding", inherited=False),
            StyleStruct("Page", inherited=False),
            StyleStruct("Position", inherited=False),
            StyleStruct("SVG", inherited=False, gecko_name="SVGReset"),
            StyleStruct("Table", inherited=False),
            StyleStruct("Text", inherited=False, gecko_name="TextReset"),
            StyleStruct("UI", inherited=False, gecko_name="UIReset"),
            StyleStruct("XUL", inherited=False),
        ]
        self.current_style_struct = None

    def active_style_structs(self):
        return [s for s in self.style_structs if s.longhands]

    def add_prefixed_aliases(self, property):
        # FIXME Servo's DOM architecture doesn't support vendor-prefixed properties.
        #       See servo/servo#14941.
        if self.engine == "gecko":
            for prefix, pref in property.extra_prefixes:
                property.aliases.append(("-%s-%s" % (prefix, property.name), pref))

    def declare_longhand(self, name, engines=None, **kwargs):
        engines = engines.split()
        if self.engine not in engines:
            return

        longhand = Longhand(self.current_style_struct, name, **kwargs)
        self.add_prefixed_aliases(longhand)
        longhand.aliases = [Alias(xp[0], longhand, xp[1]) for xp in longhand.aliases]
        self.longhand_aliases += longhand.aliases
        self.current_style_struct.longhands.append(longhand)
        self.longhands.append(longhand)
        self.longhands_by_name[name] = longhand
        if longhand.logical_group:
            self.longhands_by_logical_group.setdefault(
                longhand.logical_group, []
            ).append(longhand)

        return longhand

    def declare_shorthand(self, name, sub_properties, engines, *args, **kwargs):
        engines = engines.split()
        if self.engine not in engines:
            return

        sub_properties = [self.longhands_by_name[s] for s in sub_properties]
        shorthand = Shorthand(name, sub_properties, *args, **kwargs)
        self.add_prefixed_aliases(shorthand)
        shorthand.aliases = [Alias(xp[0], shorthand, xp[1]) for xp in shorthand.aliases]
        self.shorthand_aliases += shorthand.aliases
        self.shorthands.append(shorthand)
        self.shorthands_by_name[name] = shorthand
        return shorthand

    def shorthands_except_all(self):
        return [s for s in self.shorthands if s.name != "all"]

    def all_aliases(self):
        return self.longhand_aliases + self.shorthand_aliases


def _add_logical_props(data, props):
    groups = set()
    for prop in props:
        if prop not in data.longhands_by_name:
            assert data.engine == "servo"
            continue
        prop = data.longhands_by_name[prop]
        if prop.logical_group:
            groups.add(prop.logical_group)
    for group in groups:
        for prop in data.longhands_by_logical_group[group]:
            props.add(prop.name)


# These are probably Gecko bugs and should be supported per spec.
def _remove_common_first_line_and_first_letter_properties(props, engine):
    if engine == "gecko":
        props.remove("tab-size")
        props.remove("hyphens")
        props.remove("line-break")
        props.remove("text-align-last")
        props.remove("text-emphasis-position")
        props.remove("text-emphasis-style")
        props.remove("text-emphasis-color")
        props.remove("text-wrap-style")

    props.remove("overflow-wrap")
    props.remove("text-align")
    props.remove("text-justify")
    props.remove("white-space-collapse")
    props.remove("text-wrap-mode")
    props.remove("word-break")
    props.remove("text-indent")


class PropertyRestrictions:
    @staticmethod
    def logical_group(data, group):
        return [p.name for p in data.longhands_by_logical_group[group]]

    @staticmethod
    def shorthand(data, shorthand):
        if shorthand not in data.shorthands_by_name:
            return []
        return [p.name for p in data.shorthands_by_name[shorthand].sub_properties]

    @staticmethod
    def spec(data, spec_path):
        return [p.name for p in data.longhands if spec_path in p.spec]

    # https://svgwg.org/svg2-draft/propidx.html
    @staticmethod
    def svg_text_properties():
        props = set(
            [
                "fill",
                "fill-opacity",
                "fill-rule",
                "paint-order",
                "stroke",
                "stroke-dasharray",
                "stroke-dashoffset",
                "stroke-linecap",
                "stroke-linejoin",
                "stroke-miterlimit",
                "stroke-opacity",
                "stroke-width",
                "text-rendering",
                "vector-effect",
            ]
        )
        return props

    @staticmethod
    def webkit_text_properties():
        props = set(
            [
                # Kinda like css-text?
                "-webkit-text-stroke-width",
                "-webkit-text-fill-color",
                "-webkit-text-stroke-color",
            ]
        )
        return props

    # https://drafts.csswg.org/css-pseudo/#first-letter-styling
    @staticmethod
    def first_letter(data):
        props = set(
            [
                "color",
                "opacity",
                "float",
                "initial-letter",
                # Kinda like css-fonts?
                "-moz-osx-font-smoothing",
                "vertical-align",
                # Will become shorthand of vertical-align (Bug 1830771)
                "baseline-source",
                "line-height",
                # Kinda like css-backgrounds?
                "background-blend-mode",
            ]
            + PropertyRestrictions.shorthand(data, "padding")
            + PropertyRestrictions.shorthand(data, "margin")
            + PropertyRestrictions.spec(data, "css-fonts")
            + PropertyRestrictions.spec(data, "css-backgrounds")
            + PropertyRestrictions.spec(data, "css-text")
            + PropertyRestrictions.spec(data, "css-shapes")
            + PropertyRestrictions.spec(data, "css-text-decor")
        )
        props = props.union(PropertyRestrictions.svg_text_properties())
        props = props.union(PropertyRestrictions.webkit_text_properties())

        _add_logical_props(data, props)

        _remove_common_first_line_and_first_letter_properties(props, data.engine)
        return props

    # https://drafts.csswg.org/css-pseudo/#first-line-styling
    @staticmethod
    def first_line(data):
        props = set(
            [
                # Per spec.
                "color",
                "opacity",
                # Kinda like css-fonts?
                "-moz-osx-font-smoothing",
                "vertical-align",
                # Will become shorthand of vertical-align (Bug 1830771)
                "baseline-source",
                "line-height",
                # Kinda like css-backgrounds?
                "background-blend-mode",
            ]
            + PropertyRestrictions.spec(data, "css-fonts")
            + PropertyRestrictions.spec(data, "css-backgrounds")
            + PropertyRestrictions.spec(data, "css-text")
            + PropertyRestrictions.spec(data, "css-text-decor")
        )
        props = props.union(PropertyRestrictions.svg_text_properties())
        props = props.union(PropertyRestrictions.webkit_text_properties())

        # These are probably Gecko bugs and should be supported per spec.
        for prop in PropertyRestrictions.shorthand(data, "border"):
            props.remove(prop)
        for prop in PropertyRestrictions.shorthand(data, "border-radius"):
            props.remove(prop)
        props.remove("box-shadow")

        _remove_common_first_line_and_first_letter_properties(props, data.engine)
        return props

    # https://drafts.csswg.org/css-pseudo/#placeholder
    #
    # The spec says that placeholder and first-line have the same restrictions,
    # but that's not true in Gecko and we also allow a handful other properties
    # for ::placeholder.
    @staticmethod
    def placeholder(data):
        props = PropertyRestrictions.first_line(data)
        props.add("opacity")
        props.add("text-overflow")
        props.add("text-align")
        props.add("text-justify")
        for p in PropertyRestrictions.shorthand(data, "text-wrap"):
            props.add(p)
        for p in PropertyRestrictions.shorthand(data, "white-space"):
            props.add(p)
        # ::placeholder can't be SVG text
        props -= PropertyRestrictions.svg_text_properties()

        return props

    # https://drafts.csswg.org/css-pseudo/#marker-pseudo
    @staticmethod
    def marker(data):
        return set(
            [
                "color",
                "text-combine-upright",
                "text-transform",
                "unicode-bidi",
                "direction",
                "content",
                "line-height",
                "-moz-osx-font-smoothing",
            ]
            + PropertyRestrictions.shorthand(data, "text-wrap")
            + PropertyRestrictions.shorthand(data, "white-space")
            + PropertyRestrictions.spec(data, "css-fonts")
            + PropertyRestrictions.spec(data, "css-animations")
            + PropertyRestrictions.spec(data, "css-transitions")
        )

    # https://www.w3.org/TR/webvtt1/#the-cue-pseudo-element
    @staticmethod
    def cue(data):
        return set(
            [
                "color",
                "opacity",
                "visibility",
                "text-shadow",
                "text-combine-upright",
                "ruby-position",
                # XXX Should these really apply to cue?
                "-moz-osx-font-smoothing",
                # FIXME(emilio): background-blend-mode should be part of the
                # background shorthand, and get reset, per
                # https://drafts.fxtf.org/compositing/#background-blend-mode
                "background-blend-mode",
            ]
            + PropertyRestrictions.shorthand(data, "text-decoration")
            + PropertyRestrictions.shorthand(data, "text-wrap")
            + PropertyRestrictions.shorthand(data, "white-space")
            + PropertyRestrictions.shorthand(data, "background")
            + PropertyRestrictions.shorthand(data, "outline")
            + PropertyRestrictions.shorthand(data, "font")
            + PropertyRestrictions.shorthand(data, "font-synthesis")
        )


class CountedUnknownProperty:
    def __init__(self, name):
        self.name = name
        self.ident = to_rust_ident(name)
        self.camel_case = to_camel_case(self.ident)
