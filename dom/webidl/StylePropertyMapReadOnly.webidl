/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://drafts.css-houdini.org/css-typed-om-1/#the-stylepropertymap
 */

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
// TODO: Expose to LayoutWorklet
[Exposed=(Window, Worker, PaintWorklet), Pref="layout.css.typed-om.enabled"]
interface StylePropertyMapReadOnly {
  iterable<UTF8String, sequence<CSSStyleValue>>;
  [Throws] (undefined or CSSStyleValue) get(UTF8String property);
  [Throws] sequence<CSSStyleValue> getAll(UTF8String property);
  [Throws] boolean has(UTF8String property);
  readonly attribute unsigned long size;
};
