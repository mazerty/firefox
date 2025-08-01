/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://wicg.github.io/scroll-to-text-fragment/
 */
[Exposed=Window, Pref="dom.text_fragments.enabled"]
interface FragmentDirective {
    [Pref="dom.text_fragments.enabled", ChromeOnly]
    sequence<Range> getTextDirectiveRanges();

    [Pref="dom.text_fragments.enabled", ChromeOnly, Throws]
    undefined removeAllTextDirectives();

    [Pref="dom.text_fragments.create_text_fragment.enabled", ChromeOnly]
    Promise<DOMString> createTextDirectiveForSelection();
};
