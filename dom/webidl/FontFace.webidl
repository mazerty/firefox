/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * http://dev.w3.org/csswg/css-font-loading/#fontface-interface
 *
 * Copyright © 2014 W3C® (MIT, ERCIM, Keio, Beihang), All Rights Reserved. W3C
 * liability, trademark and document use rules apply.
 */

typedef (ArrayBuffer or ArrayBufferView) BinaryData;

dictionary FontFaceDescriptors {
  UTF8String style = "normal";
  UTF8String weight = "normal";
  UTF8String stretch = "normal";
  UTF8String unicodeRange = "U+0-10FFFF";
  UTF8String variant = "normal";
  UTF8String featureSettings = "normal";
  [Pref="layout.css.font-variations.enabled"] UTF8String variationSettings = "normal";
  UTF8String display = "auto";
  UTF8String ascentOverride = "normal";
  UTF8String descentOverride = "normal";
  UTF8String lineGapOverride = "normal";
  UTF8String sizeAdjust = "100%";
};

enum FontFaceLoadStatus { "unloaded", "loading", "loaded", "error" };

[Exposed=(Window,Worker)]
interface FontFace {
  [Throws]
  constructor(UTF8String family,
              (UTF8String or BinaryData) source,
              optional FontFaceDescriptors descriptors = {});

  [SetterThrows] attribute UTF8String family;
  [SetterThrows] attribute UTF8String style;
  [SetterThrows] attribute UTF8String weight;
  [SetterThrows] attribute UTF8String stretch;
  [SetterThrows] attribute UTF8String unicodeRange;
  [SetterThrows] attribute UTF8String variant;
  [SetterThrows] attribute UTF8String featureSettings;
  [SetterThrows, Pref="layout.css.font-variations.enabled"] attribute UTF8String variationSettings;
  [SetterThrows] attribute UTF8String display;
  [SetterThrows] attribute UTF8String ascentOverride;
  [SetterThrows] attribute UTF8String descentOverride;
  [SetterThrows] attribute UTF8String lineGapOverride;
  [SetterThrows] attribute UTF8String sizeAdjust;

  readonly attribute FontFaceLoadStatus status;

  [Throws]
  Promise<FontFace> load();

  [Throws]
  readonly attribute Promise<FontFace> loaded;
};
