/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

use crate::context::QuirksMode;
use crate::error_reporting::{ContextualParseError, ParseErrorReporter};
use crate::media_queries::{Device, MediaList};
use crate::parser::ParserContext;
use crate::shared_lock::{DeepCloneWithLock, Locked};
use crate::shared_lock::{SharedRwLock, SharedRwLockReadGuard};
use crate::stylesheets::loader::StylesheetLoader;
use crate::stylesheets::rule_parser::{State, TopLevelRuleParser};
use crate::stylesheets::rules_iterator::{EffectiveRules, EffectiveRulesIterator};
use crate::stylesheets::rules_iterator::{NestedRuleIterationCondition, RulesIterator};
use crate::stylesheets::{CssRule, CssRules, Origin, UrlExtraData};
use crate::use_counters::UseCounters;
use crate::{Namespace, Prefix};
use cssparser::{Parser, ParserInput, StyleSheetParser};
use fxhash::FxHashMap;
#[cfg(feature = "gecko")]
use malloc_size_of::{MallocSizeOfOps, MallocUnconditionalShallowSizeOf};
use parking_lot::RwLock;
use servo_arc::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use style_traits::ParsingMode;

use super::scope_rule::ImplicitScopeRoot;

/// This structure holds the user-agent and user stylesheets.
pub struct UserAgentStylesheets {
    /// The lock used for user-agent stylesheets.
    pub shared_lock: SharedRwLock,
    /// The user or user agent stylesheets.
    pub user_or_user_agent_stylesheets: Vec<DocumentStyleSheet>,
    /// The quirks mode stylesheet.
    pub quirks_mode_stylesheet: DocumentStyleSheet,
}

/// A set of namespaces applying to a given stylesheet.
///
/// The namespace id is used in gecko
#[derive(Clone, Debug, Default, MallocSizeOf)]
#[allow(missing_docs)]
pub struct Namespaces {
    pub default: Option<Namespace>,
    pub prefixes: FxHashMap<Prefix, Namespace>,
}

/// The contents of a given stylesheet. This effectively maps to a
/// StyleSheetInner in Gecko.
#[derive(Debug)]
pub struct StylesheetContents {
    /// List of rules in the order they were found (important for
    /// cascading order)
    pub rules: Arc<Locked<CssRules>>,
    /// The origin of this stylesheet.
    pub origin: Origin,
    /// The url data this stylesheet should use.
    pub url_data: RwLock<UrlExtraData>,
    /// The namespaces that apply to this stylesheet.
    pub namespaces: RwLock<Namespaces>,
    /// The quirks mode of this stylesheet.
    pub quirks_mode: QuirksMode,
    /// This stylesheet's source map URL.
    pub source_map_url: RwLock<Option<String>>,
    /// This stylesheet's source URL.
    pub source_url: RwLock<Option<String>>,
    /// The use counters of the original stylesheet.
    pub use_counters: UseCounters,

    /// We don't want to allow construction outside of this file, to guarantee
    /// that all contents are created with Arc<>.
    _forbid_construction: (),
}

impl StylesheetContents {
    /// Parse a given CSS string, with a given url-data, origin, and
    /// quirks mode.
    pub fn from_str(
        css: &str,
        url_data: UrlExtraData,
        origin: Origin,
        shared_lock: &SharedRwLock,
        stylesheet_loader: Option<&dyn StylesheetLoader>,
        error_reporter: Option<&dyn ParseErrorReporter>,
        quirks_mode: QuirksMode,
        allow_import_rules: AllowImportRules,
        sanitization_data: Option<&mut SanitizationData>,
    ) -> Arc<Self> {
        let use_counters = UseCounters::default();
        let (namespaces, rules, source_map_url, source_url) = Stylesheet::parse_rules(
            css,
            &url_data,
            origin,
            &shared_lock,
            stylesheet_loader,
            error_reporter,
            quirks_mode,
            Some(&use_counters),
            allow_import_rules,
            sanitization_data,
        );

        Arc::new(Self {
            rules: CssRules::new(rules, &shared_lock),
            origin,
            url_data: RwLock::new(url_data),
            namespaces: RwLock::new(namespaces),
            quirks_mode,
            source_map_url: RwLock::new(source_map_url),
            source_url: RwLock::new(source_url),
            use_counters,
            _forbid_construction: (),
        })
    }

    /// Creates a new StylesheetContents with the specified pre-parsed rules,
    /// origin, URL data, and quirks mode.
    ///
    /// Since the rules have already been parsed, and the intention is that
    /// this function is used for read only User Agent style sheets, an empty
    /// namespace map is used, and the source map and source URLs are set to
    /// None.
    ///
    /// An empty namespace map should be fine, as it is only used for parsing,
    /// not serialization of existing selectors.  Since UA sheets are read only,
    /// we should never need the namespace map.
    pub fn from_shared_data(
        rules: Arc<Locked<CssRules>>,
        origin: Origin,
        url_data: UrlExtraData,
        quirks_mode: QuirksMode,
    ) -> Arc<Self> {
        debug_assert!(rules.is_static());
        Arc::new(Self {
            rules,
            origin,
            url_data: RwLock::new(url_data),
            namespaces: RwLock::new(Namespaces::default()),
            quirks_mode,
            source_map_url: RwLock::new(None),
            source_url: RwLock::new(None),
            use_counters: UseCounters::default(),
            _forbid_construction: (),
        })
    }

    /// Returns a reference to the list of rules.
    #[inline]
    pub fn rules<'a, 'b: 'a>(&'a self, guard: &'b SharedRwLockReadGuard) -> &'a [CssRule] {
        &self.rules.read_with(guard).0
    }

    /// Measure heap usage.
    #[cfg(feature = "gecko")]
    pub fn size_of(&self, guard: &SharedRwLockReadGuard, ops: &mut MallocSizeOfOps) -> usize {
        if self.rules.is_static() {
            return 0;
        }
        // Measurement of other fields may be added later.
        self.rules.unconditional_shallow_size_of(ops) +
            self.rules.read_with(guard).size_of(guard, ops)
    }
}

impl DeepCloneWithLock for StylesheetContents {
    fn deep_clone_with_lock(
        &self,
        lock: &SharedRwLock,
        guard: &SharedRwLockReadGuard,
    ) -> Self {
        // Make a deep clone of the rules, using the new lock.
        let rules = self
            .rules
            .read_with(guard)
            .deep_clone_with_lock(lock, guard);

        Self {
            rules: Arc::new(lock.wrap(rules)),
            quirks_mode: self.quirks_mode,
            origin: self.origin,
            url_data: RwLock::new((*self.url_data.read()).clone()),
            namespaces: RwLock::new((*self.namespaces.read()).clone()),
            source_map_url: RwLock::new((*self.source_map_url.read()).clone()),
            source_url: RwLock::new((*self.source_url.read()).clone()),
            use_counters: self.use_counters.clone(),
            _forbid_construction: (),
        }
    }
}

/// The structure servo uses to represent a stylesheet.
#[derive(Debug)]
pub struct Stylesheet {
    /// The contents of this stylesheet.
    pub contents: Arc<StylesheetContents>,
    /// The lock used for objects inside this stylesheet
    pub shared_lock: SharedRwLock,
    /// List of media associated with the Stylesheet.
    pub media: Arc<Locked<MediaList>>,
    /// Whether this stylesheet should be disabled.
    pub disabled: AtomicBool,
}

/// A trait to represent a given stylesheet in a document.
pub trait StylesheetInDocument: ::std::fmt::Debug {
    /// Get whether this stylesheet is enabled.
    fn enabled(&self) -> bool;

    /// Get the media associated with this stylesheet.
    fn media<'a>(&'a self, guard: &'a SharedRwLockReadGuard) -> Option<&'a MediaList>;

    /// Returns a reference to the list of rules in this stylesheet.
    fn rules<'a, 'b: 'a>(&'a self, guard: &'b SharedRwLockReadGuard) -> &'a [CssRule] {
        self.contents().rules(guard)
    }

    /// Returns a reference to the contents of the stylesheet.
    fn contents(&self) -> &StylesheetContents;

    /// Return an iterator using the condition `C`.
    #[inline]
    fn iter_rules<'a, 'b, C>(
        &'a self,
        device: &'a Device,
        guard: &'a SharedRwLockReadGuard<'b>,
    ) -> RulesIterator<'a, 'b, C>
    where
        C: NestedRuleIterationCondition,
    {
        let contents = self.contents();
        RulesIterator::new(
            device,
            contents.quirks_mode,
            guard,
            contents.rules(guard).iter(),
        )
    }

    /// Returns whether the style-sheet applies for the current device.
    fn is_effective_for_device(&self, device: &Device, guard: &SharedRwLockReadGuard) -> bool {
        match self.media(guard) {
            Some(medialist) => medialist.evaluate(device, self.contents().quirks_mode),
            None => true,
        }
    }

    /// Return an iterator over the effective rules within the style-sheet, as
    /// according to the supplied `Device`.
    #[inline]
    fn effective_rules<'a, 'b>(
        &'a self,
        device: &'a Device,
        guard: &'a SharedRwLockReadGuard<'b>,
    ) -> EffectiveRulesIterator<'a, 'b> {
        self.iter_rules::<EffectiveRules>(device, guard)
    }

    /// Return the implicit scope root for this stylesheet, if one exists.
    fn implicit_scope_root(&self) -> Option<ImplicitScopeRoot>;
}

impl StylesheetInDocument for Stylesheet {
    fn media<'a>(&'a self, guard: &'a SharedRwLockReadGuard) -> Option<&'a MediaList> {
        Some(self.media.read_with(guard))
    }

    fn enabled(&self) -> bool {
        !self.disabled()
    }

    #[inline]
    fn contents(&self) -> &StylesheetContents {
        &self.contents
    }

    fn implicit_scope_root(&self) -> Option<ImplicitScopeRoot> {
        None
    }
}

/// A simple wrapper over an `Arc<Stylesheet>`, with pointer comparison, and
/// suitable for its use in a `StylesheetSet`.
#[derive(Clone, Debug)]
#[cfg_attr(feature = "servo", derive(MallocSizeOf))]
pub struct DocumentStyleSheet(
    #[cfg_attr(feature = "servo", ignore_malloc_size_of = "Arc")] pub Arc<Stylesheet>,
);

impl PartialEq for DocumentStyleSheet {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}

impl StylesheetInDocument for DocumentStyleSheet {
    fn media<'a>(&'a self, guard: &'a SharedRwLockReadGuard) -> Option<&'a MediaList> {
        self.0.media(guard)
    }

    fn enabled(&self) -> bool {
        self.0.enabled()
    }

    #[inline]
    fn contents(&self) -> &StylesheetContents {
        self.0.contents()
    }

    fn implicit_scope_root(&self) -> Option<ImplicitScopeRoot> {
        None
    }
}

/// The kind of sanitization to use when parsing a stylesheet.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SanitizationKind {
    /// Perform no sanitization.
    None,
    /// Allow only @font-face, style rules, and @namespace.
    Standard,
    /// Allow everything but conditional rules.
    NoConditionalRules,
}

/// Whether @import rules are allowed.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum AllowImportRules {
    /// @import rules will be parsed.
    Yes,
    /// @import rules will not be parsed.
    No,
}

impl SanitizationKind {
    fn allows(self, rule: &CssRule) -> bool {
        debug_assert_ne!(self, SanitizationKind::None);
        // NOTE(emilio): If this becomes more complex (not filtering just by
        // top-level rules), we should thread all the data through nested rules
        // and such. But this doesn't seem necessary at the moment.
        let is_standard = matches!(self, SanitizationKind::Standard);
        match *rule {
            CssRule::Document(..) |
            CssRule::Media(..) |
            CssRule::Supports(..) |
            CssRule::Import(..) |
            CssRule::Container(..) |
            // TODO(emilio): Perhaps Layer should not be always sanitized? But
            // we sanitize @media and co, so this seems safer for now.
            CssRule::LayerStatement(..) |
            CssRule::LayerBlock(..) |
            // TODO(dshin): Same comment as Layer applies - shouldn't give away
            // something like display size - erring on the side of "safe" for now.
            CssRule::Scope(..) |
            CssRule::StartingStyle(..) => false,

            CssRule::FontFace(..) |
            CssRule::Namespace(..) |
            CssRule::Style(..) |
            CssRule::NestedDeclarations(..) |
            CssRule::PositionTry(..) => true,

            CssRule::Keyframes(..) |
            CssRule::Page(..) |
            CssRule::Margin(..) |
            CssRule::Property(..) |
            CssRule::FontFeatureValues(..) |
            CssRule::FontPaletteValues(..) |
            CssRule::CounterStyle(..) => !is_standard,
        }
    }
}

/// A struct to hold the data relevant to style sheet sanitization.
#[derive(Debug)]
pub struct SanitizationData {
    kind: SanitizationKind,
    output: String,
}

impl SanitizationData {
    /// Create a new input for sanitization.
    #[inline]
    pub fn new(kind: SanitizationKind) -> Option<Self> {
        if matches!(kind, SanitizationKind::None) {
            return None;
        }
        Some(Self {
            kind,
            output: String::new(),
        })
    }

    /// Take the sanitized output.
    #[inline]
    pub fn take(self) -> String {
        self.output
    }
}

impl Stylesheet {
    /// Updates an empty stylesheet from a given string of text.
    pub fn update_from_str(
        existing: &Stylesheet,
        css: &str,
        url_data: UrlExtraData,
        stylesheet_loader: Option<&dyn StylesheetLoader>,
        error_reporter: Option<&dyn ParseErrorReporter>,
        allow_import_rules: AllowImportRules,
    ) {
        let use_counters = UseCounters::default();
        let (namespaces, rules, source_map_url, source_url) = Self::parse_rules(
            css,
            &url_data,
            existing.contents.origin,
            &existing.shared_lock,
            stylesheet_loader,
            error_reporter,
            existing.contents.quirks_mode,
            Some(&use_counters),
            allow_import_rules,
            /* sanitization_data = */ None,
        );

        *existing.contents.url_data.write() = url_data;
        *existing.contents.namespaces.write() = namespaces;

        // Acquire the lock *after* parsing, to minimize the exclusive section.
        let mut guard = existing.shared_lock.write();
        *existing.contents.rules.write_with(&mut guard) = CssRules(rules);
        *existing.contents.source_map_url.write() = source_map_url;
        *existing.contents.source_url.write() = source_url;
        existing.contents.use_counters.merge(&use_counters);
    }

    fn parse_rules(
        css: &str,
        url_data: &UrlExtraData,
        origin: Origin,
        shared_lock: &SharedRwLock,
        stylesheet_loader: Option<&dyn StylesheetLoader>,
        error_reporter: Option<&dyn ParseErrorReporter>,
        quirks_mode: QuirksMode,
        use_counters: Option<&UseCounters>,
        allow_import_rules: AllowImportRules,
        mut sanitization_data: Option<&mut SanitizationData>,
    ) -> (Namespaces, Vec<CssRule>, Option<String>, Option<String>) {
        let mut input = ParserInput::new(css);
        let mut input = Parser::new(&mut input);

        let context = ParserContext::new(
            origin,
            url_data,
            None,
            ParsingMode::DEFAULT,
            quirks_mode,
            /* namespaces = */ Default::default(),
            error_reporter,
            use_counters,
        );

        let mut rule_parser = TopLevelRuleParser {
            shared_lock,
            loader: stylesheet_loader,
            context,
            state: State::Start,
            dom_error: None,
            insert_rule_context: None,
            allow_import_rules,
            declaration_parser_state: Default::default(),
            first_declaration_block: Default::default(),
            wants_first_declaration_block: false,
            error_reporting_state: Default::default(),
            rules: Vec::new(),
        };

        {
            let mut iter = StyleSheetParser::new(&mut input, &mut rule_parser);
            while let Some(result) = iter.next() {
                match result {
                    Ok(rule_start) => {
                        // TODO(emilio, nesting): sanitize nested CSS rules, probably?
                        if let Some(ref mut data) = sanitization_data {
                            if let Some(ref rule) = iter.parser.rules.last() {
                                if !data.kind.allows(rule) {
                                    iter.parser.rules.pop();
                                    continue;
                                }
                            }
                            let end = iter.input.position().byte_index();
                            data.output.push_str(&css[rule_start.byte_index()..end]);
                        }
                    },
                    Err((error, slice)) => {
                        let location = error.location;
                        let error = ContextualParseError::InvalidRule(slice, error);
                        iter.parser.context.log_css_error(location, error);
                    },
                }
            }
        }

        let source_map_url = input.current_source_map_url().map(String::from);
        let source_url = input.current_source_url().map(String::from);
        (
            rule_parser.context.namespaces.into_owned(),
            rule_parser.rules,
            source_map_url,
            source_url,
        )
    }

    /// Creates an empty stylesheet and parses it with a given base url, origin
    /// and media.
    ///
    /// Effectively creates a new stylesheet and forwards the hard work to
    /// `Stylesheet::update_from_str`.
    pub fn from_str(
        css: &str,
        url_data: UrlExtraData,
        origin: Origin,
        media: Arc<Locked<MediaList>>,
        shared_lock: SharedRwLock,
        stylesheet_loader: Option<&dyn StylesheetLoader>,
        error_reporter: Option<&dyn ParseErrorReporter>,
        quirks_mode: QuirksMode,
        allow_import_rules: AllowImportRules,
    ) -> Self {
        // FIXME: Consider adding use counters to Servo?
        let contents = StylesheetContents::from_str(
            css,
            url_data,
            origin,
            &shared_lock,
            stylesheet_loader,
            error_reporter,
            quirks_mode,
            allow_import_rules,
            /* sanitized_output = */ None,
        );

        Stylesheet {
            contents,
            shared_lock,
            media,
            disabled: AtomicBool::new(false),
        }
    }

    /// Returns whether the stylesheet has been explicitly disabled through the
    /// CSSOM.
    pub fn disabled(&self) -> bool {
        self.disabled.load(Ordering::SeqCst)
    }

    /// Records that the stylesheet has been explicitly disabled through the
    /// CSSOM.
    ///
    /// Returns whether the the call resulted in a change in disabled state.
    ///
    /// Disabled stylesheets remain in the document, but their rules are not
    /// added to the Stylist.
    pub fn set_disabled(&self, disabled: bool) -> bool {
        self.disabled.swap(disabled, Ordering::SeqCst) != disabled
    }
}

#[cfg(feature = "servo")]
impl Clone for Stylesheet {
    fn clone(&self) -> Self {
        // Create a new lock for our clone.
        let lock = self.shared_lock.clone();
        let guard = self.shared_lock.read();

        // Make a deep clone of the media, using the new lock.
        let media = self.media.read_with(&guard).clone();
        let media = Arc::new(lock.wrap(media));
        let contents = Arc::new(self.contents.deep_clone_with_lock(&lock, &guard));

        Stylesheet {
            contents,
            media,
            shared_lock: lock,
            disabled: AtomicBool::new(self.disabled.load(Ordering::SeqCst)),
        }
    }
}
