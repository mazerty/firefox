/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import { CardGrid } from "content-src/components/DiscoveryStreamComponents/CardGrid/CardGrid";
import { CollectionCardGrid } from "content-src/components/DiscoveryStreamComponents/CollectionCardGrid/CollectionCardGrid";
import { CollapsibleSection } from "content-src/components/CollapsibleSection/CollapsibleSection";
import { connect } from "react-redux";
import { DSMessage } from "content-src/components/DiscoveryStreamComponents/DSMessage/DSMessage";
import { DSPrivacyModal } from "content-src/components/DiscoveryStreamComponents/DSPrivacyModal/DSPrivacyModal";
import { ReportContent } from "../DiscoveryStreamComponents/ReportContent/ReportContent";
import { DSSignup } from "content-src/components/DiscoveryStreamComponents/DSSignup/DSSignup";
import { DSTextPromo } from "content-src/components/DiscoveryStreamComponents/DSTextPromo/DSTextPromo";
import { Highlights } from "content-src/components/DiscoveryStreamComponents/Highlights/Highlights";
import { HorizontalRule } from "content-src/components/DiscoveryStreamComponents/HorizontalRule/HorizontalRule";
import { Navigation } from "content-src/components/DiscoveryStreamComponents/Navigation/Navigation";
import { PrivacyLink } from "content-src/components/DiscoveryStreamComponents/PrivacyLink/PrivacyLink";
import React from "react";
import { SectionTitle } from "content-src/components/DiscoveryStreamComponents/SectionTitle/SectionTitle";
import { selectLayoutRender } from "content-src/lib/selectLayoutRender";
import { TopSites } from "content-src/components/TopSites/TopSites";
import { CardSections } from "../DiscoveryStreamComponents/CardSections/CardSections";
import { Widgets } from "content-src/components/Widgets/Widgets";

const ALLOWED_CSS_URL_PREFIXES = [
  "chrome://",
  "resource://",
  "https://img-getpocket.cdn.mozilla.net/",
];
const DUMMY_CSS_SELECTOR = "DUMMY#CSS.SELECTOR";

/**
 * Validate a CSS declaration. The values are assumed to be normalized by CSSOM.
 */
export function isAllowedCSS(property, value) {
  // Bug 1454823: INTERNAL properties, e.g., -moz-context-properties, are
  // exposed but their values aren't resulting in getting nothing. Fortunately,
  // we don't care about validating the values of the current set of properties.
  if (value === undefined) {
    return true;
  }

  // Make sure all urls are of the allowed protocols/prefixes
  const urls = value.match(/url\("[^"]+"\)/g);
  return (
    !urls ||
    urls.every(url =>
      ALLOWED_CSS_URL_PREFIXES.some(prefix => url.slice(5).startsWith(prefix))
    )
  );
}

export class _DiscoveryStreamBase extends React.PureComponent {
  constructor(props) {
    super(props);
    this.onStyleMount = this.onStyleMount.bind(this);
  }

  onStyleMount(style) {
    // Unmounting style gets rid of old styles, so nothing else to do
    if (!style) {
      return;
    }

    const { sheet } = style;
    const styles = JSON.parse(style.dataset.styles);
    styles.forEach((row, rowIndex) => {
      row.forEach((component, componentIndex) => {
        // Nothing to do without optional styles overrides
        if (!component) {
          return;
        }

        Object.entries(component).forEach(([selectors, declarations]) => {
          // Start with a dummy rule to validate declarations and selectors
          sheet.insertRule(`${DUMMY_CSS_SELECTOR} {}`);
          const [rule] = sheet.cssRules;

          // Validate declarations and remove any offenders. CSSOM silently
          // discards invalid entries, so here we apply extra restrictions.
          rule.style = declarations;
          [...rule.style].forEach(property => {
            const value = rule.style[property];
            if (!isAllowedCSS(property, value)) {
              console.error(`Bad CSS declaration ${property}: ${value}`);
              rule.style.removeProperty(property);
            }
          });

          // Set the actual desired selectors scoped to the component
          const prefix = `.ds-layout > .ds-column:nth-child(${
            rowIndex + 1
          }) .ds-column-grid > :nth-child(${componentIndex + 1})`;
          // NB: Splitting on "," doesn't work with strings with commas, but
          // we're okay with not supporting those selectors
          rule.selectorText = selectors
            .split(",")
            .map(
              selector =>
                prefix +
                // Assume :pseudo-classes are for component instead of descendant
                (selector[0] === ":" ? "" : " ") +
                selector
            )
            .join(",");

          // CSSOM silently ignores bad selectors, so we'll be noisy instead
          if (rule.selectorText === DUMMY_CSS_SELECTOR) {
            console.error(`Bad CSS selector ${selectors}`);
          }
        });
      });
    });
  }

  renderComponent(component) {
    switch (component.type) {
      case "Highlights":
        return <Highlights />;
      case "TopSites":
        return (
          <div className="ds-top-sites">
            <TopSites isFixed={true} title={component.header?.title} />
          </div>
        );
      case "TextPromo":
        return (
          <DSTextPromo
            dispatch={this.props.dispatch}
            type={component.type}
            data={component.data}
          />
        );
      case "Signup":
        return (
          <DSSignup
            dispatch={this.props.dispatch}
            type={component.type}
            data={component.data}
          />
        );
      case "Message":
        return (
          <DSMessage
            title={component.header && component.header.title}
            subtitle={component.header && component.header.subtitle}
            link_text={component.header && component.header.link_text}
            link_url={component.header && component.header.link_url}
            icon={component.header && component.header.icon}
            essentialReadsHeader={component.essentialReadsHeader}
            editorsPicksHeader={component.editorsPicksHeader}
          />
        );
      case "SectionTitle":
        return <SectionTitle header={component.header} />;
      case "Navigation":
        return (
          <Navigation
            dispatch={this.props.dispatch}
            links={component.properties.links}
            extraLinks={component.properties.extraLinks}
            alignment={component.properties.alignment}
            explore_topics={component.properties.explore_topics}
            header={component.header}
            locale={this.props.App.locale}
            newFooterSection={component.newFooterSection}
            privacyNoticeURL={component.properties.privacyNoticeURL}
          />
        );
      case "CollectionCardGrid": {
        const { DiscoveryStream } = this.props;
        return (
          <CollectionCardGrid
            data={component.data}
            feed={component.feed}
            spocs={DiscoveryStream.spocs}
            placement={component.placement}
            type={component.type}
            items={component.properties.items}
            dismissible={this.props.DiscoveryStream.isCollectionDismissible}
            dispatch={this.props.dispatch}
          />
        );
      }
      case "CardGrid": {
        const sectionsEnabled =
          this.props.Prefs.values["discoverystream.sections.enabled"];
        if (sectionsEnabled) {
          return (
            <CardSections
              feed={component.feed}
              data={component.data}
              dispatch={this.props.dispatch}
              type={component.type}
              firstVisibleTimestamp={this.props.firstVisibleTimestamp}
              is_collection={true}
              ctaButtonSponsors={component.properties.ctaButtonSponsors}
              ctaButtonVariant={component.properties.ctaButtonVariant}
              spocMessageVariant={component.properties.spocMessageVariant}
            />
          );
        }
        return (
          <CardGrid
            title={component.header && component.header.title}
            data={component.data}
            feed={component.feed}
            widgets={component.widgets}
            type={component.type}
            dispatch={this.props.dispatch}
            items={component.properties.items}
            hybridLayout={component.properties.hybridLayout}
            hideCardBackground={component.properties.hideCardBackground}
            fourCardLayout={component.properties.fourCardLayout}
            compactGrid={component.properties.compactGrid}
            essentialReadsHeader={component.properties.essentialReadsHeader}
            onboardingExperience={component.properties.onboardingExperience}
            ctaButtonSponsors={component.properties.ctaButtonSponsors}
            ctaButtonVariant={component.properties.ctaButtonVariant}
            spocMessageVariant={component.properties.spocMessageVariant}
            editorsPicksHeader={component.properties.editorsPicksHeader}
            recentSavesEnabled={this.props.DiscoveryStream.recentSavesEnabled}
            hideDescriptions={this.props.DiscoveryStream.hideDescriptions}
            firstVisibleTimestamp={this.props.firstVisibleTimestamp}
            spocPositions={component.spocs?.positions}
          />
        );
      }
      case "HorizontalRule":
        return <HorizontalRule />;
      case "PrivacyLink":
        return <PrivacyLink properties={component.properties} />;
      case "Widgets": {
        // Nimbus experiment override
        const nimbusWidgetsEnabled =
          this.props.Prefs.values.widgetsConfig?.enabled;

        const widgetsEnabled =
          this.props.Prefs.values["widgets.system.enabled"];
        if (widgetsEnabled || nimbusWidgetsEnabled) {
          return <Widgets />;
        }
        return null;
      }
      default:
        return <div>{component.type}</div>;
    }
  }

  renderStyles(styles) {
    // Use json string as both the key and styles to render so React knows when
    // to unmount and mount a new instance for new styles.
    const json = JSON.stringify(styles);
    return <style key={json} data-styles={json} ref={this.onStyleMount} />;
  }

  render() {
    const { locale, mayHaveSponsoredStories } = this.props;
    // Bug 1980459 - Note that selectLayoutRender acts as a selector that transforms layout data based on current
    // preferences and experiment flags. It runs after Redux state is populated but before render.
    // Components removed in selectLayoutRender (e.g., Widgets or TopSites) will not appear in the
    // layoutRender result, and therefore will not be rendered here regardless of logic below.

    // Select layout renders data by adding spocs and position to recommendations
    const { layoutRender } = selectLayoutRender({
      state: this.props.DiscoveryStream,
      prefs: this.props.Prefs.values,
      locale,
    });
    const sectionsEnabled =
      this.props.Prefs.values["discoverystream.sections.enabled"];
    const { config } = this.props.DiscoveryStream;
    const topicSelectionEnabled =
      this.props.Prefs.values["discoverystream.topicSelection.enabled"];
    const reportAdsEnabled =
      this.props.Prefs.values["discoverystream.reportAds.enabled"];
    const spocsEnabled = this.props.Prefs.values["unifiedAds.spocs.enabled"];

    // Allow rendering without extracting special components
    if (!config.collapsible) {
      return this.renderLayout(layoutRender);
    }

    // Find the first component of a type and remove it from layout
    const extractComponent = type => {
      for (const [rowIndex, row] of Object.entries(layoutRender)) {
        for (const [index, component] of Object.entries(row.components)) {
          if (component.type === type) {
            // Remove the row if it was the only component or the single item
            if (row.components.length === 1) {
              layoutRender.splice(rowIndex, 1);
            } else {
              row.components.splice(index, 1);
            }
            return component;
          }
        }
      }
      return null;
    };

    // Get "topstories" Section state for default values
    const topStories = this.props.Sections.find(s => s.id === "topstories");

    if (!topStories) {
      return null;
    }

    // Extract TopSites to render before the rest and Message to use for header
    const topSites = extractComponent("TopSites");
    const widgets = extractComponent("Widgets");
    const sponsoredCollection = extractComponent("CollectionCardGrid");
    const message = extractComponent("Message") || {
      header: {
        link_text: topStories.learnMore.link.message,
        link_url: topStories.learnMore.link.href,
        title: topStories.title,
      },
    };

    const privacyLinkComponent = extractComponent("PrivacyLink");
    let learnMore = {
      link: {
        href: message.header.link_url,
        message: message.header.link_text,
      },
    };
    let sectionTitle = message.header.title;
    let subTitle = "";

    // If we're in one of these experiments, override the default message.
    // For now this is English only.
    if (message.essentialReadsHeader || message.editorsPicksHeader) {
      learnMore = null;
      subTitle = "Recommended By Pocket";
      if (message.essentialReadsHeader) {
        sectionTitle = "Today’s Essential Reads";
      } else if (message.editorsPicksHeader) {
        sectionTitle = "Editor’s Picks";
      }
    }

    const { DiscoveryStream } = this.props;

    return (
      <React.Fragment>
        {this.props.DiscoveryStream.isPrivacyInfoModalVisible && (
          <DSPrivacyModal dispatch={this.props.dispatch} />
        )}

        {/* Reporting stories/articles will only be available in sections, not the default card grid  */}
        {((reportAdsEnabled && spocsEnabled) || sectionsEnabled) && (
          <ReportContent spocs={DiscoveryStream.spocs} />
        )}

        {topSites &&
          this.renderLayout([
            {
              width: 12,
              components: [topSites],
              sectionType: "topsites",
            },
          ])}
        {widgets &&
          this.renderLayout([
            {
              width: 12,
              components: [widgets],
              sectionType: "widgets",
            },
          ])}
        {sponsoredCollection &&
          this.renderLayout([
            {
              width: 12,
              components: [sponsoredCollection],
            },
          ])}
        {!!layoutRender.length && (
          <CollapsibleSection
            className="ds-layout"
            collapsed={topStories.pref.collapsed}
            dispatch={this.props.dispatch}
            id={topStories.id}
            isFixed={true}
            learnMore={learnMore}
            privacyNoticeURL={topStories.privacyNoticeURL}
            showPrefName={topStories.pref.feed}
            title={sectionTitle}
            subTitle={subTitle}
            mayHaveSponsoredStories={mayHaveSponsoredStories}
            mayHaveTopicsSelection={topicSelectionEnabled}
            sectionsEnabled={sectionsEnabled}
            spocMessageVariant={message?.properties?.spocMessageVariant}
            eventSource="CARDGRID"
          >
            {this.renderLayout(layoutRender)}
          </CollapsibleSection>
        )}
        {this.renderLayout([
          {
            width: 12,
            components: [{ type: "Highlights" }],
          },
        ])}
        {privacyLinkComponent &&
          this.renderLayout([
            {
              width: 12,
              components: [privacyLinkComponent],
            },
          ])}
      </React.Fragment>
    );
  }

  renderLayout(layoutRender) {
    const styles = [];
    let [data] = layoutRender;
    // Add helper class for topsites
    const sectionClass = data.sectionType
      ? `ds-layout-${data.sectionType}`
      : "";

    return (
      <div className={`discovery-stream ds-layout ${sectionClass}`}>
        {layoutRender.map((row, rowIndex) => (
          <div
            key={`row-${rowIndex}`}
            className={`ds-column ds-column-${row.width}`}
          >
            <div className="ds-column-grid">
              {row.components.map((component, componentIndex) => {
                if (!component) {
                  return null;
                }
                styles[rowIndex] = [
                  ...(styles[rowIndex] || []),
                  component.styles,
                ];
                return (
                  <div key={`component-${componentIndex}`}>
                    {this.renderComponent(component, row.width)}
                  </div>
                );
              })}
            </div>
          </div>
        ))}
        {this.renderStyles(styles)}
      </div>
    );
  }
}

export const DiscoveryStreamBase = connect(state => ({
  DiscoveryStream: state.DiscoveryStream,
  Prefs: state.Prefs,
  Sections: state.Sections,
  document: globalThis.document,
  App: state.App,
}))(_DiscoveryStreamBase);
